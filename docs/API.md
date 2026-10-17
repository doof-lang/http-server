# std/http-server Guide

`std/http-server` is an inbound HTTP primitive. Native socket I/O accepts and
parses requests, then posts immutable `Request` snapshots to the handler passed
to `Server.listen`. The handler runs on the actor that created it and completes
each exchange through the responder embedded in the request.

## Request Flow

1. The native reactor accepts and parses a complete request.
2. The server posts the `Request` to the handler with `tryPost`.
3. The handler runs on its owning actor, in arrival order.
4. The handler calls `request.respond(response)` or
   `request.upgradeToWebSocket(connection)`.

Responses do not go back through the handler. If the handler's actor mailbox is
full, or the actor has retired, the server rejects the request with
`503 Service Unavailable`. An open server keeps `runMainEventLoop()` running
unless `ServerOptions.keepsAlive` is `false`.

## Requests And Responses

Request bodies are buffered up to `ServerOptions.maxBodyBytes`. Oversized bodies
are rejected before dispatch. `Request` exposes helpers for headers, raw bytes,
UTF-8 text, and JSON parsing.

Buffered response helpers cover empty, blob, text, HTML, and JSON payloads.
`Response.stream` sends a `Stream<readonly byte[]>` with HTTP/1.1 chunked
transfer encoding.

Response compression can be automatic, disabled, or requested explicitly. Zstd
is preferred over gzip when the client accepts both. Existing `Content-Encoding`
headers are respected.

## WebSockets

WebSocket upgrades are explicit. Check `Request.isWebSocketUpgrade()`, create a
connection with `createWebSocketConnection(onEvent, options)`, then call
`request.upgradeToWebSocket(connection)`. Events are posted to `onEvent` in
order; writes are `sendText`, `sendBinary`, `ping`, and `close` on the
connection.

Native reads pause once `WebSocketOptions.eventCapacity` events are posted but
not yet handled, and resume when half have been handled. WebSocket message size
is bounded by `ServerOptions.maxBodyBytes`.

## Timeouts And Limits

`idleTimeoutMillis` bounds idle keep-alive connections and incomplete request
reads. `responseTimeoutMillis` bounds delivered requests whose handler never
responds or upgrades. `maxRequestsPerConnection` can cap sequential HTTP/1.1
requests per connection.

The first implementation does not support streaming request bodies, WebSocket
compression extensions, or concurrent HTTP/1.1 pipelining.

## API Map

Server:

- `Server`
- `ServerOptions`
- `ServerError`

HTTP:

- `Request`
- `Response`
- `ResponseCompression`
- header helpers

WebSockets:

- `WebSocketOptions`
- `WebSocketConnection`
- `WebSocketEvent` and its event classes
- `createWebSocketConnection`

Declarations are defined across [index.do](../index.do), [server.do](../server.do),
[request.do](../request.do), [response.do](../response.do), and [websocket.do](../websocket.do).
