# std/http-server Guide

`std/http-server` is an inbound HTTP primitive. Native socket I/O accepts and
parses requests, then sends immutable `Request` snapshots through a
`std/event.ChannelSender<Request>`. Application code owns the receiver endpoint
and completes each exchange through the responder embedded in the request.

## Request Flow

1. The native reactor accepts and parses a complete request.
2. The server enqueues a `Request` on the configured sender.
3. Application handlers receive the request on the event loop.
4. The handler calls `request.respond(response)` or
   `request.upgradeToWebSocket(connection)`.

The channel is not bidirectional. Responses do not go back through the channel.
If the request channel is full or closed, the server rejects the request with
`503 Service Unavailable`.

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
connection with `createWebSocketConnection`, subscribe to its event channel, then
call `request.upgradeToWebSocket(connection)`.

Inbound WebSocket events and outbound commands use bounded channels. Backpressure
pauses native reads when the event channel is full. WebSocket message size is
bounded by `ServerOptions.maxBodyBytes`.

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
- `WebSocketEvent`
- `WebSocketCommand`
- `createWebSocketConnection`
- send/close/ping command types

Declarations are defined across [index.do](../index.do), [server.do](../server.do),
[request.do](../request.do), [response.do](../response.do), and [websocket.do](../websocket.do).
