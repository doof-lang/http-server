import { BlobBuilder } from "std/blob"
import { HttpHeader } from "std/http"

import { ServerError, mapNativeVoid, parseServerError } from "./errors"
import { headersAreSafe, renderHeaders } from "./headers"
import { NativeResponder, NativeWebSocketConnection, NativeWebSocketEvent } from "./native"
import { validateWebSocketHandshake } from "./websocket_internal"

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

export function upgradeNativeResponderToWebSocket(
  nativeResponder: NativeResponder,
  method: string,
  version: string,
  requestHeadersText: string,
  connection: WebSocketConnection,
): void {
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
      connection.handler(WebSocketError {
        connection,
        error: parseServerError(f.error),
      })
      return
    }
  }

  subprotocol := connection.options.subprotocol ?? ""
  nativeResponder.upgradeToWebSocket(
    connection.native,
    websocketHandshakeResponseText(accept, renderHeaders(connection.options.headers), subprotocol),
    (event: NativeWebSocketEvent): void => {
      connection.handler(nativeWebSocketEventToPublic(connection, event))
    },
  )
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
