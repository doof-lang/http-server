import { BlobBuilder, BlobReader } from "std/blob"
import { AsyncEventChannel, AsyncEventChannelError } from "std/event"
import { HttpHeader } from "std/http"
import { formatJsonValue, parseJsonValue } from "std/json"

import class NativeExchange from "./native_http_server.hpp" as doof_http_server::NativeExchange {
  method(): string
  target(): string
  version(): string
  headersText(): string
  body(): readonly byte[]
  responder(): NativeResponder
}

import class NativeResponder from "./native_http_server.hpp" as doof_http_server::NativeResponder {
  respond(status: int, headersText: string, body: readonly byte[]): Result<void, string>
  upgradeWebSocket(
    websocket: NativeWebSocketConnection,
    headersText: string,
    subprotocol: string,
    callback: (event: NativeWebSocketEvent): void,
  ): void
}

import class NativeWebSocketEvent from "./native_http_server.hpp" as doof_http_server::NativeWebSocketEvent {
  kind(): int
  text(): string
  bytes(): readonly byte[]
  code(): int
  reason(): string
  wasClean(): bool
  error(): string
}

import class NativeWebSocketConnection from "./native_http_server.hpp" as doof_http_server::NativeWebSocketConnection {
  static create(): NativeWebSocketConnection
  sendText(text: string): Result<void, string>
  sendBinary(bytes: readonly byte[]): Result<void, string>
  ping(): Result<void, string>
  close(code: int, reason: string): Result<void, string>
  state(): int
}

import class NativeHttpServer from "./native_http_server.hpp" as doof_http_server::NativeHttpServer {
  static listen(
    host: string,
    port: int,
    maxBodyBytes: long,
    idleTimeoutMillis: int,
    responseTimeoutMillis: int,
    maxRequestsPerConnection: int,
    onRequest: (exchange: NativeExchange): int,
  ): Result<NativeHttpServer, string>

  host(): string
  port(): int
  close(): Result<void, string>
}

export class ServerOptions {
  readonly host: string = "127.0.0.1"
  readonly port: int
  readonly maxBodyBytes: long = 1_048_576L
  readonly idleTimeoutMillis: int = 30_000
  readonly responseTimeoutMillis: int = 30_000
  readonly maxRequestsPerConnection: int = 0
}

export class ServerError {
  readonly kind: string
  readonly message: string
}

export enum WebSocketState {
  Connecting,
  Open,
  Closing,
  Closed,
  Error,
}

export const WEBSOCKET_CLOSE_NORMAL = 1000
export const WEBSOCKET_CLOSE_GOING_AWAY = 1001
export const WEBSOCKET_CLOSE_PROTOCOL_ERROR = 1002
export const WEBSOCKET_CLOSE_UNSUPPORTED_DATA = 1003
export const WEBSOCKET_CLOSE_INVALID_PAYLOAD = 1007
export const WEBSOCKET_CLOSE_POLICY_VIOLATION = 1008
export const WEBSOCKET_CLOSE_MESSAGE_TOO_BIG = 1009
export const WEBSOCKET_CLOSE_INTERNAL_ERROR = 1011

export class WebSocketOptions {
  readonly eventCapacity: int = 1024
  readonly headers: readonly HttpHeader[] = []
  readonly subprotocol: string | null = null
}

export type WebSocketEvent =
  WebSocketOpen |
  WebSocketText |
  WebSocketBinary |
  WebSocketWritable |
  WebSocketClose |
  WebSocketError

export class WebSocketOpen {
  readonly connection: WebSocketConnection
}

export class WebSocketText {
  readonly connection: WebSocketConnection
  readonly text: string
}

export class WebSocketBinary {
  readonly connection: WebSocketConnection
  readonly bytes: readonly byte[]
}

export class WebSocketWritable {
  readonly connection: WebSocketConnection
}

export class WebSocketClose {
  readonly connection: WebSocketConnection
  readonly code: int
  readonly reason: string
  readonly wasClean: bool
}

export class WebSocketError {
  readonly connection: WebSocketConnection
  readonly error: ServerError
}

export class WebSocketConnection {
  readonly handler: (event: WebSocketEvent): void
  readonly options: WebSocketOptions = WebSocketOptions {}
  private readonly native: NativeWebSocketConnection = NativeWebSocketConnection()

  sendText(text: string): Result<void, ServerError> {
    return mapNativeVoid(this.native.sendText(text))
  }

  sendBinary(bytes: readonly byte[]): Result<void, ServerError> {
    return mapNativeVoid(this.native.sendBinary(bytes))
  }

  ping(): Result<void, ServerError> {
    return mapNativeVoid(this.native.ping())
  }

  close(
    code: int = 1000,
    reason: string = "",
  ): Result<void, ServerError> {
    return mapNativeVoid(this.native.close(code, reason))
  }

  state(): WebSocketState {
    return nativeStateToPublic(this.native.state())
  }
}

export class Request {
  readonly method: string
  readonly target: string
  readonly path: string
  readonly queryString: string
  readonly version: string
  readonly headers: readonly HttpHeader[]
  readonly body: readonly byte[]
  private readonly responder: NativeResponder

  header(name: string): string | null {
    lowerName := name.toLowerCase()
    for entry of this.headers {
      if entry.name.toLowerCase() == lowerName {
        return entry.value
      }
    }
    return null
  }

  headerValues(name: string): readonly string[] {
    lowerName := name.toLowerCase()
    values: string[] := []
    for entry of this.headers {
      if entry.name.toLowerCase() == lowerName {
        values.push(entry.value)
      }
    }
    return values.buildReadonly()
  }

  getBlob(): readonly byte[] {
    return this.body
  }

  getText(): string {
    reader := BlobReader(this.body)
    return reader.readString(reader.remaining())
  }

  getJsonValue(): Result<JsonValue, string> {
    return parseJsonValue(this.getText())
  }

  respond(response: Response): Result<void, ServerError> {
    if !headersAreSafe(response.headers) {
      return Failure {
        error: ServerError {
          kind: "invalid-header",
          message: "Response headers cannot contain CR or LF characters",
        }
      }
    }

    return mapNativeVoid(this.responder.respond(
      response.status,
      renderHeaders(response.headers),
      response.body,
    ))
  }

  upgradeWebSocket(connection: WebSocketConnection): void {
    if !headersAreSafe(connection.options.headers) {
      connection.handler(WebSocketError {
        connection,
        error: ServerError {
          kind: "invalid-header",
          message: "WebSocket response headers cannot contain CR or LF characters",
        },
      })
      return
    }

    subprotocol := connection.options.subprotocol ?? ""
    this.responder.upgradeWebSocket(
      connection.native,
      renderHeaders(connection.options.headers),
      subprotocol,
      (event: NativeWebSocketEvent): void => {
        connection.handler(nativeWebSocketEventToPublic(connection, event))
      },
    )
  }
}

export class Response {
  readonly status: int
  readonly headers: readonly HttpHeader[]
  readonly body: readonly byte[]

  static empty(status: int = 204): Response {
    return Response {
      status,
      headers: readonly [],
      body: readonly [],
    }
  }

  static blob(
    status: int,
    body: readonly byte[],
    headers: readonly HttpHeader[] = [],
  ): Response {
    return Response {
      status,
      headers,
      body,
    }
  }

  static text(
    status: int,
    body: string,
    headers: readonly HttpHeader[] = [],
  ): Response {
    return Response.blob(
      status,
      encodeText(body),
      withDefaultContentType(headers, "text/plain; charset=utf-8"),
    )
  }

  static html(
    status: int,
    body: string,
    headers: readonly HttpHeader[] = [],
  ): Response {
    return Response.blob(
      status,
      encodeText(body),
      withDefaultContentType(headers, "text/html; charset=utf-8"),
    )
  }

  static jsonValue(
    status: int,
    body: JsonValue,
    headers: readonly HttpHeader[] = [],
  ): Response {
    return Response.blob(
      status,
      encodeText(formatJsonValue(body)),
      withDefaultContentType(headers, "application/json; charset=utf-8"),
    )
  }
}

export class Server {
  readonly host: string
  readonly port: int
  private readonly native: NativeHttpServer

  static listen(
    options: ServerOptions,
    requests: AsyncEventChannel<Request>,
  ): Result<Server, ServerError> {
    started := NativeHttpServer.listen(
      options.host,
      options.port,
      options.maxBodyBytes,
      options.idleTimeoutMillis,
      options.responseTimeoutMillis,
      options.maxRequestsPerConnection,
      (exchange: NativeExchange): int => requestDisposition(requests, exchange),
    )

    return case started {
      s: Success -> Success {
        value: Server {
          host: s.value.host(),
          port: s.value.port(),
          native: s.value,
        }
      },
      f: Failure -> Failure {
        error: parseServerError(f.error)
      }
    }
  }

  close(): Result<void, ServerError> {
    return mapNativeVoid(this.native.close())
  }
}

function requestDisposition(
  requests: AsyncEventChannel<Request>,
  exchange: NativeExchange,
): int {
  delivered := requests.send(requestFromExchange(exchange))
  return case delivered {
    _: Success -> 0,
    f: Failure -> case f.error {
      AsyncEventChannelError.Full -> 1,
      _ -> 2,
    }
  }
}

function requestFromExchange(exchange: NativeExchange): Request {
  target := exchange.target()
  querySeparator := target.indexOf("?")
  let path = target
  let queryString = ""
  if querySeparator >= 0 {
    path = target.substring(0, querySeparator)
    queryString = target.slice(querySeparator + 1)
  }
  if path == "" {
    path = "/"
  }

  return Request {
    method: exchange.method(),
    target,
    path,
    queryString,
    version: exchange.version(),
    headers: parseHeaders(exchange.headersText()),
    body: exchange.body(),
    responder: exchange.responder(),
  }
}

function parseHeaders(headerText: string): readonly HttpHeader[] {
  headers: HttpHeader[] := []
  lines := headerText.split("\r\n")
  for line of lines {
    if line == "" {
      continue
    }

    separator := line.indexOf(":")
    if separator <= 0 {
      continue
    }

    headers.push(HttpHeader {
      name: line.substring(0, separator).trim(),
      value: line.slice(separator + 1).trim(),
    })
  }
  return headers.buildReadonly()
}

function nativeStateToPublic(state: int): WebSocketState {
  return case state {
    0 -> WebSocketState.Connecting,
    1 -> WebSocketState.Open,
    2 -> WebSocketState.Closing,
    3 -> WebSocketState.Closed,
    _ -> WebSocketState.Error,
  }
}

function nativeWebSocketEventToPublic(
  connection: WebSocketConnection,
  event: NativeWebSocketEvent,
): WebSocketEvent {
  return case event.kind() {
    0 -> WebSocketOpen {
      connection,
    },
    1 -> WebSocketText {
      connection,
      text: event.text(),
    },
    2 -> WebSocketBinary {
      connection,
      bytes: event.bytes(),
    },
    3 -> WebSocketWritable {
      connection,
    },
    4 -> WebSocketClose {
      connection,
      code: event.code(),
      reason: event.reason(),
      wasClean: event.wasClean(),
    },
    _ -> WebSocketError {
      connection,
      error: parseServerError(event.error()),
    },
  }
}

function renderHeaders(headers: readonly HttpHeader[]): string {
  let text = ""
  for header of headers {
    text += "${header.name}: ${header.value}\r\n"
  }
  return text
}

function headersAreSafe(headers: readonly HttpHeader[]): bool {
  for header of headers {
    if header.name.contains("\r") || header.name.contains("\n") {
      return false
    }
    if header.value.contains("\r") || header.value.contains("\n") {
      return false
    }
  }
  return true
}

function hasHeader(headers: readonly HttpHeader[], name: string): bool {
  lowerName := name.toLowerCase()
  for header of headers {
    if header.name.toLowerCase() == lowerName {
      return true
    }
  }
  return false
}

function withDefaultContentType(
  headers: readonly HttpHeader[],
  contentType: string,
): readonly HttpHeader[] {
  if hasHeader(headers, "Content-Type") {
    return headers
  }

  merged: HttpHeader[] := []
  for header of headers {
    merged.push(header)
  }
  merged.push(HttpHeader {
    name: "Content-Type",
    value: contentType,
  })
  return merged.buildReadonly()
}

function encodeText(text: string): readonly byte[] {
  builder := BlobBuilder()
  builder.writeString(text)
  return builder.build()
}

function parseServerError(raw: string): ServerError {
  separator := raw.indexOf("|")
  if separator < 0 {
    return ServerError {
      kind: "server",
      message: raw,
    }
  }

  return ServerError {
    kind: raw.substring(0, separator),
    message: raw.slice(separator + 1),
  }
}

function mapNativeVoid(result: Result<void, string>): Result<void, ServerError> {
  return case result {
    _: Success -> Success {},
    f: Failure -> Failure {
      error: parseServerError(f.error)
    }
  }
}
