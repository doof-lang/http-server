# Multithreaded HTTP server

A `std/http-server` program that handles requests in parallel on a pool of
worker actors, plus a load generator that measures it across worker and
connection counts.

| File | Role |
| --- | --- |
| `server.do` | Parses options and starts the server with `Server.listenPool` |
| `pool.do` | `Worker` (one per actor) and the root-domain `WorkerPool`, which owns the workers and handles `/admin/*` |
| `workload.do` | Isolated request handling: `/hello`, `/cpu`, `/sleep` |
| `loadtest.do` | Concurrent client actors that sweep worker and connection counts |

## How it works

`Server.listen` posts every request to one callback, so a server with a single
handler runs one request at a time. This sample uses `Server.listenPool`
instead, which posts each request straight to one of several worker actors:

1. At startup `WorkerPool.resize(n)` creates `n` `Actor<Worker>` values. Each
   worker returns a `RequestHandler` from `handler()`, made with
   `requestHandler(...)` inside the worker's actor, so requests posted to it
   run `Worker.handle` there.
2. `Server.listenPool` receives the handlers. Its native I/O threads pick the
   next handler in round-robin order for each request and post to it
   directly; the root domain is not on the request path.
3. The worker calls the isolated `respondTo` function and then
   `request.respond(...)`. The response is written on the worker's thread
   through the responder inside the request.
4. Posting never blocks the I/O threads. A worker whose mailbox is full is
   skipped; if every worker is full the server answers `503` right away
   instead of queueing without limit.
5. `/admin/*` requests reach a worker like any other. The worker forwards them
   to a root-domain callback owned by `WorkerPool`, which can change pool
   state and calls `server.setHandlers(...)` to resize the pool.

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
does, because a sleeping worker cannot start its next request. `Thread.sleep`
keeps its runtime CPU token while it sleeps, though, so no more workers than
there are cores can sleep at once: past the core count, extra workers do not
raise `/sleep` throughput.

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
  alike) while using about three cores. `ab -k -c 32` reaches about
  140,000 req/s against a release (`doof package`) server, and a minimal
  C++ kqueue server reaches about 210,000 req/s, so on loopback `ab` itself
  is close to its limit.
- To measure the server alone, use a keep-alive benchmark client. In the same
  setup, `ab` reached about 1,275, 4,850, and 9,200 req/s at
  `/cpu?n=200000` with 1, 4, and 8 workers:

  ```bash
  ab -k -c 8 -n 6000 'http://127.0.0.1:8080/cpu?n=200000'
  ```
