import { CliSpec } from "std/cli"
import { HttpClient, HttpRequest, createClient, send } from "std/http"
import { parseInt } from "std/parse"
import { Duration, MonotonicInstant } from "std/time"

// What one simulated client observed during a run. Readonly so it can travel
// from the client's actor back to the root domain.
class ClientStats {
  readonly ok: int
  readonly rejected: int
  readonly failed: int
  readonly firstError: string
  readonly busyMicros: long
  readonly latenciesMicros: readonly long[]
}

// One simulated client: its own actor and its own HttpClient, issuing
// requests back to back until its time is up.
//
// A blocking send gives up its runtime CPU token while it waits, so clients
// waiting on the server do not stop other clients from running. runOnce
// reports the concurrency actually achieved.
class LoadClient {
  client: HttpClient = createClient()

  run(url: string, millis: int): ClientStats {
    begin := MonotonicInstant.now()
    deadline := begin.plus(Duration.ofMillis(long(millis)))
    latencies: long[] := []
    let ok = 0
    let rejected = 0
    let failed = 0
    let firstError = ""
    while MonotonicInstant.now().isBefore(deadline) {
      started := MonotonicInstant.now()
      case send(this.client, HttpRequest { method: "GET", url, timeoutMs: 10_000 }) {
        s: Success -> {
          if s.value.status == 200 {
            ok += 1
            latencies.push(MonotonicInstant.now().durationSince(started).toMicros())
          } else if s.value.status == 503 {
            rejected += 1
          } else {
            failed += 1
            if firstError == "" { firstError = "HTTP ${s.value.status}" }
          }
        },
        f: Failure -> {
          failed += 1
          if firstError == "" { firstError = f.error.message }
        },
      }
    }
    return ClientStats {
      ok,
      rejected,
      failed,
      firstError,
      busyMicros: MonotonicInstant.now().durationSince(begin).toMicros(),
      latenciesMicros: latencies.drainToReadonly(),
    }
  }
}

function parseCounts(text: string, name: string): Result<int[], string> {
  counts: int[] := []
  for part of text.split(",") {
    count := parseInt(part.trim()) else {
      return Failure("--${name} expects comma-separated integers, got '${part}'")
    }
    if count < 1 {
      return Failure("--${name} values must be positive")
    }
    counts.push(count)
  }
  return Success(counts)
}

function percentile(sorted: long[], fraction: double): long {
  if sorted.length == 0 {
    return 0L
  }
  index := int(double(sorted.length - 1) * fraction)
  return sorted[index]
}

function millisText(micros: long): string {
  return "${micros \ 1000L}.${string(micros % 1000L \ 100L)}"
}

function column(text: string, width: int): string => text.padStart(width)

// Asks the server to route new requests to `count` workers.
function setWorkers(client: HttpClient, base: string, count: int): Result<none, string> {
  response := send(client, HttpRequest { method: "POST", url: "${base}/admin/workers?count=${count}" }) else error {
    return Failure("could not reach ${base}: ${error.message}")
  }
  if response.status != 200 {
    return Failure("server refused ${count} workers: HTTP ${response.status}")
  }
  return Success()
}

// Runs `connections` clients against `url` for `millis` and prints one row.
function runOnce(clients: Actor<LoadClient>[], workers: int, connections: int, url: string, millis: int): none {
  begin := MonotonicInstant.now()
  pending: Promise<ClientStats>[] := []
  for i of 0..<connections {
    pending.push(async clients[i].run(url, millis))
  }

  latencies: long[] := []
  let ok = 0
  let rejected = 0
  let failed = 0
  let busyMicros = 0L
  let firstError = ""
  for promise of pending {
    stats := promise.get() else error {
      println("client failed: ${error}")
      continue
    }
    ok += stats.ok
    rejected += stats.rejected
    failed += stats.failed
    busyMicros += stats.busyMicros
    if firstError == "" { firstError = stats.firstError }
    for latency of stats.latenciesMicros {
      latencies.push(latency)
    }
  }
  latencies.sort(=> if a < b then -1 else if a > b then 1 else 0)

  // Use real elapsed time, not `millis`: clients beyond the CPU-token limit
  // start late, which stretches the run. `active` is the average number of
  // clients that were actually sending; if it is well below `conns`, the
  // load generator, not the server, is the limit.
  wallMicros := MonotonicInstant.now().durationSince(begin).toMicros()
  requestsPerSecond := long(double(ok) * 1_000_000.0 / double(wallMicros))
  active := long(double(busyMicros) * 10.0 / double(wallMicros))
  println(
    column("${workers}", 7) + column("${connections}", 7) + column("${active \ 10L}.${active % 10L}", 8)
      + column("${requestsPerSecond}", 10)
      + column(millisText(percentile(latencies, 0.5)), 9)
      + column(millisText(percentile(latencies, 0.95)), 9)
      + column(millisText(percentile(latencies, 0.99)), 9)
      + column("${rejected}", 8) + column("${failed}", 7)
  )
  if firstError != "" {
    println("               first error: ${firstError}")
  }
}

function main(args: string[]): int {
  spec := CliSpec.create("loadtest", "Measure the multithreaded server across worker and connection counts")
    .option{ name: "url", short: "u", valueName: "URL", description: "server base URL", defaultValue: "http://127.0.0.1:8080" }
    .option{ name: "path", valueName: "PATH", description: "request path and query", defaultValue: "/cpu?n=2000000" }
    .option{ name: "workers", short: "w", valueName: "LIST", description: "worker counts to sweep", defaultValue: "1,2,4,8" }
    .option{ name: "connections", short: "c", valueName: "LIST", description: "concurrent clients to sweep", defaultValue: "1,4,8" }
    .option{ name: "millis", short: "m", valueName: "MS", description: "duration of each run", defaultValue: "2000" }
  options := spec.parse(args) else error {
    println(error.message)
    println(error.usage)
    return 2
  }

  base := options.string("url") ?? "http://127.0.0.1:8080"
  path := options.string("path") ?? "/cpu?n=2000000"
  workerCounts := parseCounts(options.string("workers") ?? "1,2,4,8", "workers") else error {
    println(error)
    return 2
  }
  connectionCounts := parseCounts(options.string("connections") ?? "1,4,8", "connections") else error {
    println(error)
    return 2
  }
  millis := parseInt(options.string("millis") ?? "2000") else {
    println("--millis must be an integer")
    return 2
  }

  // Create enough client actors for the largest run up front; runs reuse
  // them so connection setup is not part of the measurement.
  let maxConnections = 0
  for count of connectionCounts {
    if count > maxConnections {
      maxConnections = count
    }
  }
  clients: Actor<LoadClient>[] := []
  for _ of 0..<maxConnections {
    clients.push(Actor<LoadClient>())
  }

  control := createClient()
  url := base + path
  println("GET ${url}, ${millis}ms per run")
  println("workers  conns  active     req/s   p50 ms   p95 ms   p99 ms     503 errors")
  for workers of workerCounts {
    setWorkers(control, base, workers) else error {
      println(error)
      return 1
    }
    for connections of connectionCounts {
      runOnce(clients, workers, connections, url, millis)
    }
  }

  stats := send(control, HttpRequest { method: "GET", url: "${base}/admin/stats" }) else error {
    println("could not read server stats: ${error.message}")
    return 1
  }
  println("server: ${stats.getText().trim()}")
  return 0
}
