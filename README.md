# std/http-server

`std/http-server` is a small inbound HTTP primitive for Doof programs. A native
readiness reactor owns socket I/O, turns each accepted request into an immutable
`Request` snapshot, and sends that snapshot through a `std/event` channel. The
application owns the receiving endpoint, handles each request on the event loop,
and completes the exchange with `request.respond(...)` or
`request.upgradeToWebSocket(...)`.

The request channel is one-way. The server is a producer of `Request` values; it
does not receive responses through the channel. Responses are sent through the
per-request responder stored inside `Request`, which lets the channel remain a
bounded delivery queue rather than a bidirectional protocol.

## Usage

```doof
import { Request, Response, Server, ServerOptions } from "std/http-server"
import { createChannel, runMainEventLoop } from "std/event"

function handleRequest(request: Request): void {
  response := case request.path {
    "/health" -> Response.jsonValue(200, { status: "ok" }),
    "/" -> Response.text(200, "hello\n"),
    _ -> Response.text(404, "not found\n"),
  }

  try! request.respond(response)
}

function main(): int {
  (requestSender, requestReceiver) := createChannel<Request>{
    capacity: 256,
    keepsAlive: true,
  }

  requestReceiver.onMessage(handleRequest)

  server := try! Server.listen{
    options: ServerOptions { port: 8080 },
    requests: requestSender,
  }

  runMainEventLoop()
  try! server.close()
  return 0
}
```

Pass the sender endpoint to `Server.listen`, and attach application handlers to
the receiver endpoint. If the handler needs to coordinate with another actor or
native component, pass that component its own endpoint or create a second
channel for replies.

## Samples

- `samples/simple` serves files from a local `public` directory and shows
  basic URL path decoding, traversal rejection, content type headers, and
  directory `index.html` handling.

## Request Flow

1. The native reactor reads and parses a complete HTTP request.
2. The server attempts to enqueue a `Request` on the supplied channel sender.
3. The receiver endpoint delivers the `Request` to the application handler.
4. The handler sends the HTTP response by calling `request.respond(response)`.
5. For WebSocket upgrades, the handler calls
   `request.upgradeToWebSocket(connection)` instead of `respond(...)`.

If the request channel is full or closed, the listener rejects the request with
`503 Service Unavailable` instead of growing an unbounded queue.

## Request And Response

- `Request` and `Response` expose readonly data only.
- `Request.header(name)` returns the first matching header value, ignoring case.
- `Request.headerValues(name)` returns all matching header values.
- `Request.getBlob()`, `Request.getText()`, and `Request.getJsonValue()` expose
  the buffered request body.
- Request bodies are buffered up to `ServerOptions.maxBodyBytes`, including
  chunked transfer-encoded bodies; oversized payloads are rejected with
  `413 Payload Too Large`.
- `Response.empty(...)`, `Response.blob(...)`, `Response.text(...)`,
  `Response.html(...)`, and `Response.jsonValue(...)` create buffered
  responses.
- `Response.stream(status, chunks, headers, compression)` sends a
  `Stream<readonly byte[]>` response with HTTP/1.1 chunked transfer encoding.
  Streamed responses omit `Content-Length`, ignore empty chunks, and preserve
  keep-alive behavior after the final chunk.

`Response.compression` controls response compression. The default policy
compresses zstd- or gzip-capable clients for common textual content types. Use
`ResponseCompression.None` to opt out or `ResponseCompression.Compress` to
request compression when the client advertises `Accept-Encoding: zstd` or
`Accept-Encoding: gzip`. Zstd is preferred when both encodings are accepted.
Compression is skipped when a response already has `Content-Encoding`; streamed
compressed responses are encoded incrementally.

## WebSockets

`Request.isWebSocketUpgrade()` returns true for WebSocket upgrade attempts based
on `Upgrade: websocket` and a `Connection` header containing the `upgrade`
token.

`Request.upgradeToWebSocket(connection)` claims an HTTP request for WebSocket
upgrade using a caller-created `WebSocketConnection`. The method returns `void`;
handshake and runtime failures are reported as `WebSocketError` events through
the connection handler.

WebSocket v1 supports HTTP/1.1 RFC 6455 handshakes, text and binary messages,
ping/pong, close frames, and fragmented text/binary messages. WebSocket message
size is bounded by `ServerOptions.maxBodyBytes`.

## Server Options

```doof
class ServerOptions {
  readonly host: string = "127.0.0.1"
  readonly port: int
  readonly maxBodyBytes: long = 1_048_576L
  readonly idleTimeoutMillis: int = 30_000
  readonly responseTimeoutMillis: int = 30_000
  readonly maxRequestsPerConnection: int = 0
}
```

HTTP/1.1 connections stay open by default and can serve sequential requests;
`Connection: close` closes after the current response.

`idleTimeoutMillis` defaults to 30 seconds for otherwise-idle keep-alive
connections and incomplete request reads, which bounds slowloris-style
partial-header clients. Set it to `0` to disable idle expiry.

`responseTimeoutMillis` defaults to 30 seconds. If a request is delivered but
its handler never calls `request.respond(...)` or upgrades the request, the
server sends `504 Gateway Timeout` and closes the connection. Set it to `0` to
disable this timeout.

`maxRequestsPerConnection` defaults to `0` (unbounded). Set a positive value to
close a connection after that many completed requests.

## Platform Notes

The internal reactor has an explicit platform seam. macOS uses `kqueue`; other
POSIX platforms use a portable `poll` fallback.

The first implementation does not yet support streaming request bodies,
WebSocket compression extensions, or concurrent HTTP/1.1 pipeline handling.
Requests with unsupported `Transfer-Encoding` values are rejected with
`501 Not Implemented` before dispatch.
