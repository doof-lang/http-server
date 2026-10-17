# std/http-server

`std/http-server` is a small inbound HTTP primitive for Doof programs. A native
readiness reactor owns socket I/O, turns each accepted request into an immutable
`Request` snapshot, and posts it to the request handler passed to
`Server.listen`. The handler runs on the actor that created it, in arrival
order, and completes the exchange with `request.respond(...)` or
`request.upgradeToWebSocket(...)`.

Responses are sent through the per-request responder stored inside `Request`,
so the handler never needs a reply path back to the server.

## Documentation

- [Guide and API reference](docs/API.md) explains request delivery, response ownership, compression, WebSockets, timeouts, platform behavior, and API groups.
- Tests can be run with `doof test http-server`.
- [Samples](samples/) show complete programs built with this module.

## Usage

```doof
import { Request, Response, Server, ServerOptions } from "std/http-server"
import { runMainEventLoop } from "std/event"

function handleRequest(request: Request): void {
  response := case request.path {
    "/health" -> Response.jsonValue(200, { status: "ok" }),
    "/" -> Response.text(200, "hello\n"),
    _ -> Response.text(404, "not found\n"),
  }

  request.respond(response)!
}

function main(): int {
  server := Server.listen{
    options: ServerOptions { port: 8080 },
    onRequest: handleRequest,
  }!

  runMainEventLoop()
  server.close()!
  return 0
}
```

An open server keeps `runMainEventLoop()` running until `close()`. Create the
handler inside an actor to handle requests on that actor instead of the root
event loop; pass it callbacks of your own to report results back.

## Samples

- `samples/simple` serves files from a local `public` directory and shows
  basic URL path decoding, traversal rejection, content type headers, and
  directory `index.html` handling.

## Request Flow

1. The native reactor reads and parses a complete HTTP request.
2. The server posts the `Request` to the handler with `tryPost`.
3. The handler runs on its owning actor.
4. The handler sends the HTTP response by calling `request.respond(response)`.
5. For WebSocket upgrades, the handler calls
   `request.upgradeToWebSocket(connection)` instead of `respond(...)`.

If the handler's actor mailbox is full, or the actor has retired, the listener
rejects the request with `503 Service Unavailable` instead of growing an
unbounded queue.

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
upgrade using a caller-created `WebSocketConnection`. Create connections with
`createWebSocketConnection(onEvent, options)`. Inbound websocket events are
posted to `onEvent` in order on the actor that created it, and application
writes call `sendText`, `sendBinary`, `ping`, and `close` on the connection.

```doof
import {
  Request,
  WebSocketEvent,
  WebSocketOptions,
  WebSocketText,
  createWebSocketConnection,
} from "std/http-server"

function handleSocketEvent(event: WebSocketEvent): void {
  text := event as WebSocketText
  case text {
    s: Success -> {
      s.value.connection.sendText("echo:" + s.value.text)!
    }
    _: Failure -> {}
  }
}

function handleRequest(request: Request): void {
  if !request.isWebSocketUpgrade() {
    return
  }

  connection := createWebSocketConnection(
    handleSocketEvent,
    WebSocketOptions { eventCapacity: 1024 },
  )
  request.upgradeToWebSocket(connection)
}
```

Handshake and runtime failures are reported as `WebSocketError` events.
`WebSocketWritable` reports that queued outbound frames have been written.

Inbound events apply backpressure to the native socket reader. Once
`eventCapacity` events have been posted but not yet handled, the server pauses
read interest for that websocket connection and leaves already-buffered bytes
in place. Reads resume when the handler has worked through half of them. A
connection keeps `runMainEventLoop()` running until it closes or fails.

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
  readonly keepsAlive: bool = true
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

`keepsAlive` defaults to `true`, so an open server keeps `runMainEventLoop()`
running. Pass `false` for a passive server that only serves while something
else keeps the loop alive.

## Platform Notes

The internal reactor has an explicit platform seam. macOS uses `kqueue`; other
POSIX platforms use a portable `poll` fallback.

The first implementation does not yet support streaming request bodies,
WebSocket compression extensions, or concurrent HTTP/1.1 pipeline handling.
Requests with unsupported `Transfer-Encoding` values are rejected with
`501 Not Implemented` before dispatch.
