# Multithreaded HTTP server

A `std/http-server` program that handles requests in parallel on a pool of
worker actors, plus a load generator that measures it across worker and
connection counts.

| File | Role |
| --- | --- |
| `server.do` | Parses options, starts the server, handles `/admin/*` on the root domain |
| `pool.do` | `Worker` (one per actor) and the root-domain `WorkerPool` dispatcher |
| `workload.do` | Isolated request handling: `/hello`, `/cpu`, `/sleep` |
| `loadtest.do` | Concurrent client actors that sweep worker and connection counts |

## How it works

`Server.listen` posts every request to one callback on the actor that created
it, so a server whose callback is on the root domain handles one request at a
time. To run requests in parallel, the root callback works as a dispatcher:

1. At startup `WorkerPool.resize(n)` creates `n` `Actor<Worker>` values. Each
   worker returns a callback from `handler()`. That callback belongs to the
   worker's actor, so posting a `Request` to it runs `Worker.handle` there.
2. For each request, `WorkerPool.dispatch` picks the next worker in round-robin
   order and calls `handler.tryPost(request)`. `Request` is deeply immutable
   (its native responder is an `import isolated class`), so it can cross to
   the worker.
3. The worker calls the isolated `respondTo` function and then
   `request.respond(...)`. The response goes back through the responder inside
   the request, not through the dispatcher.
4. `tryPost` never blocks the event loop. If a worker's mailbox is full, the
   pool answers `503` right away instead of queueing without limit.

Each worker handles one request at a time, in order. Different workers run at
the same time on the runtime's shared thread pool, which by default has one CPU
token per core.

## Endpoints

| Request | Effect |
| --- | --- |
| `GET /hello` | Trivial response naming the worker |
| `GET /cpu?n=2000000` | CPU-bound hash loop on the worker (about 8 ms on Apple M-series) |
| `GET /sleep?ms=50` | Blocks the worker, like a synchronous database call |
| `POST /admin/workers?count=N` | Routes new requests to the first `N` workers |
| `GET /admin/stats` | Requests served by each worker and the number of 503s |

`/admin/workers` lets one server process be measured at several pool sizes
without restarting.

## Running it

Build both programs from this directory:

```bash
doof build server.do -o build/server
doof build loadtest.do -o build/loadtest
```

Start the server in one terminal:

```bash
./build/server/multithreaded-server-sample --workers 4 --port 8080
```

Run the default sweep in another terminal (workers 1, 2, 4, 8 × connections 1, 4, 8, at
`/cpu?n=2000000` for 2 s each):

```bash
./build/loadtest/multithreaded-server-sample
```

Options: `--url`, `--path`, `--workers 1,2,4`, `--connections 1,8`, and
`--millis 2000`. To measure blocking work instead of CPU work:

```bash
./build/loadtest/multithreaded-server-sample --path '/sleep?ms=50' --workers 1,4,8 --connections 8
```

## Sample results

These numbers are from a 15-core Apple M-series machine with debug builds:

```text
GET http://127.0.0.1:8080/cpu?n=2000000, 2000ms per run
workers  conns  active     req/s   p50 ms   p95 ms   p99 ms     503 errors
      1      1     0.9       121      8.1      8.3      9.7       0      0
      1      8     7.8       128     61.8     62.1     66.0       0      0
      2      8     7.9       253     31.5     31.7     31.7       0      0
      4      8     7.9       485     16.4     17.2     17.4       0      0
      8      4     3.9       444      8.9      9.1      9.4       0      0
      8      8     7.9       838      9.4     10.2     11.0       0      0
```

- With one connection, adding workers does not help: there is only one
  request in flight at a time.
- With eight connections, throughput doubles each time the worker count
  doubles, and p50 latency halves as requests stop waiting in a worker's
  mailbox.
- Throughput is limited by whichever is smaller, workers or connections:
  8 workers × 4 connections gives the same result as 4 × 4.

The `/sleep` endpoint scales the same way (17 → 71 → 138 req/s for 1, 4, and 8
workers at 50 ms per request). Blocking work needs workers just as CPU work
does, because a sleeping worker cannot start its next request.

## Reading the numbers

- **`active`** is the average number of clients that were actually sending.
  A client blocked inside the native HTTP call gives up its runtime CPU
  token, so all clients can be in flight at once. Requests per second are
  computed from real elapsed time. If `active` is well below `conns`, the
  load generator is the limit, not the server.
- The load generator shares the machine with the server. Once workers plus
  clients exceed the number of cores, both slow down.
- Each client keeps one HTTP connection open and reuses it, so sustained
  runs do not pile up sockets in `TIME_WAIT`.
- **The load generator is the limit for cheap requests.** On Apple
  platforms `std/http` uses `NSURLSession`, which costs about 250 µs of
  client CPU per request. With 8 workers and 12 connections at `/hello`,
  the load generator reaches about 13,000 req/s (release or debug builds
  alike) while using about three cores. In the same setup `ab -k -c 12`
  reaches about 45,000 req/s against a release (`doof package`) server.
- To measure the server alone, use a keep-alive benchmark client. In the same
  setup, `ab` reached about 1,275, 4,850, and 9,200 req/s at
  `/cpu?n=200000` with 1, 4, and 8 workers:

  ```bash
  ab -k -c 8 -n 6000 'http://127.0.0.1:8080/cpu?n=200000'
  ```
