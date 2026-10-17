import { BlobBuilder } from "std/blob"
import { HttpHeader } from "std/http"

import { ServerError, mapNativeVoid, parseServerError } from "./errors"
import { headersAreSafe, renderHeaders } from "./headers"
import { NativeResponder, NativeWebSocketConnection } from "./native"
import { validateWebSocketHandshake } from "./websocket_internal"

import isolated function _attachNativeWebSocketHandler(
  native: NativeWebSocketConnection,
  connection: WebSocketConnection,
  onEvent: (event: WebSocketEvent): none,
  eventCapacity: int,
): none from "./native_http_server.hpp" as doof_http_server::attachWebSocketHandler

export enum WebSocketState {
  Connecting,
  Open,
  Closing,
  Closed,
  Error,
}

export readonly WEBSOCKET_CLOSE_NORMAL = 1000
export readonly WEBSOCKET_CLOSE_GOING_AWAY = 1001
export readonly WEBSOCKET_CLOSE_PROTOCOL_ERROR = 1002
export readonly WEBSOCKET_CLOSE_UNSUPPORTED_DATA = 1003
export readonly WEBSOCKET_CLOSE_INVALID_PAYLOAD = 1007
export readonly WEBSOCKET_CLOSE_POLICY_VIOLATION = 1008
export readonly WEBSOCKET_CLOSE_MESSAGE_TOO_BIG = 1009
export readonly WEBSOCKET_CLOSE_INTERNAL_ERROR = 1011

export class WebSocketOptions {
  // Inbound events posted to the handler but not yet handled before the
  // connection stops reading from the socket.
  readonly eventCapacity: int = 1024
  readonly headers: readonly HttpHeader[] = []
  readonly subprotocol: string | none = none
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
  readonly options: WebSocketOptions = WebSocketOptions {}
  private readonly native: NativeWebSocketConnection

  state(): WebSocketState {
    return nativeStateToPublic(this.native.state())
  }

  sendText(text: string): Result<none, ServerError> {
    return mapNativeVoid(this.native.sendText(text))
  }

  sendBinary(bytes: readonly byte[]): Result<none, ServerError> {
    return mapNativeVoid(this.native.sendBinary(bytes))
  }

  ping(): Result<none, ServerError> {
    return mapNativeVoid(this.native.ping())
  }

  close(code: int = WEBSOCKET_CLOSE_NORMAL, reason: string = ""): Result<none, ServerError> {
    return mapNativeVoid(this.native.close(code, reason))
  }
}

// Events are delivered to `onEvent` on the actor that created it, in order.
export function createWebSocketConnection(
  onEvent: (event: WebSocketEvent): none,
  options: WebSocketOptions = WebSocketOptions {},
): WebSocketConnection {
  native := NativeWebSocketConnection()
  connection := WebSocketConnection { options, native }

  _attachNativeWebSocketHandler(
    native,
    connection,
    (event: WebSocketEvent): none => {
      onEvent(event)
      native.eventHandled()
    },
    options.eventCapacity,
  )

  return connection
}

export function upgradeNativeResponderToWebSocket(
  nativeResponder: NativeResponder,
  method: string,
  version: string,
  requestHeadersText: string,
  connection: WebSocketConnection,
): none {
  if !headersAreSafe(connection.options.headers) {
    failWebSocketConnection(
      connection,
      ServerError {
        kind: "invalid-header",
        message: "WebSocket response headers cannot contain CR or LF characters",
      },
    )
    return
  }

  checked := validateWebSocketHandshake(method, version, requestHeadersText)
  let accept = ""
  case checked {
    s: Success -> {
      accept = s.value
    }
    f: Failure -> {
      ignored := nativeResponder.respond(
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\nConnection: close\r\n\r\n",
        encodeText("Bad Request\n"),
        false,
      )
      failWebSocketConnection(connection, parseServerError(f.error))
      return
    }
  }

  subprotocol := connection.options.subprotocol ?? ""
  nativeResponder.upgradeToWebSocket(
    connection.native,
    websocketHandshakeResponseText(accept, renderHeaders(connection.options.headers), subprotocol),
  )
}

export function failWebSocketConnection(
  connection: WebSocketConnection,
  error: ServerError,
): none {
  connection.native.fail("${error.kind}|${error.message}")
}

function websocketHandshakeResponseText(
  accept: string,
  extraHeaders: string,
  subprotocol: string,
): string {
  let text = "HTTP/1.1 101 Switching Protocols\r\n"
  text += "Upgrade: websocket\r\n"
  text += "Connection: Upgrade\r\n"
  text += "Sec-WebSocket-Accept: ${accept}\r\n"
  if subprotocol != "" {
    text += "Sec-WebSocket-Protocol: ${subprotocol}\r\n"
  }
  text += extraHeaders
  text += "\r\n"
  return text
}

function encodeText(text: string): readonly byte[] {
  builder := BlobBuilder()
  builder.writeString(text)
  return builder.build()
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
