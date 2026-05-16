# std/http-server

`std/http-server` is a small inbound HTTP primitive built around
`std/event.AsyncEventChannel<Request>`. A native readiness reactor owns socket
I/O, immutable `Request` snapshots are delivered onto the supplied event
channel, and handlers complete the exchange through `request.respond(...)`.

## Usage

```doof
import { Request, Response, Server, ServerOptions } from "std/http-server"
import { createMainAsyncEventChannel, runMainEventLoop } from "std/event"

function main(): int {
  requests := createMainAsyncEventChannel<Request>{
    handler: (request: Request): void => {
      response := case request.path {
        "/health" -> Response.jsonValue(200, { status: "ok" }),
        _ -> Response.text(404, "not found\n"),
      }
      try! request.respond(response)
    },
    capacity: 256,
    keepsAlive: true,
  }

  server := try! Server.listen{
    options: ServerOptions { port: 8080 },
    requests,
  }

  runMainEventLoop()
  try! server.close()
  return 0
}
```

## Notes

- `Request` and `Response` expose readonly data only.
- Request bodies are buffered up to `ServerOptions.maxBodyBytes`; oversized
  payloads are rejected with `413 Payload Too Large`.
- If the supplied event channel is full or closed, the listener rejects the
  request with `503 Service Unavailable` instead of growing an unbounded queue.
- A native connection object owns socket lifetime independently of any one
  request handler. HTTP/1.1 connections stay open by default and can serve
  sequential requests; `Connection: close` closes after the current response.
- The internal reactor has an explicit platform seam; the first backend is a
  macOS `kqueue` implementation.
- The first implementation does not yet support streaming request or response
  bodies, chunked transfer encoding, or concurrent HTTP/1.1 pipeline handling.
