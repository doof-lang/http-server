import { BlobBuilder } from "std/blob"
import { Backpressure, ChannelReceiver, ChannelSender, SendError, createChannel } from "std/event"
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
  readonly commandCapacity: int = 1024
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

export type WebSocketCommand =
  WebSocketSendText |
  WebSocketSendBinary |
  WebSocketPing |
  WebSocketCloseCommand

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

export class WebSocketSendText {
  readonly text: string
}

export class WebSocketSendBinary {
  readonly bytes: readonly byte[]
}

export class WebSocketPing {
}

export class WebSocketCloseCommand {
  readonly code: int = 1000
  readonly reason: string = ""
}

export class WebSocketConnection {
  readonly events: ChannelReceiver<WebSocketEvent>
  readonly commands: ChannelSender<WebSocketCommand>
  readonly options: WebSocketOptions = WebSocketOptions {}
  private readonly eventSender: ChannelSender<WebSocketEvent>
  private readonly commandReceiver: ChannelReceiver<WebSocketCommand>
  private readonly native: NativeWebSocketConnection = NativeWebSocketConnection()

  state(): WebSocketState {
    return nativeStateToPublic(this.native.state())
  }

  close(): void {
    this.commands.close()
    this.events.close()
  }
}

export function createWebSocketConnection(
  options: WebSocketOptions = WebSocketOptions {},
): WebSocketConnection {
  (eventSender, events) := createChannel<WebSocketEvent>{
    capacity: options.eventCapacity,
    keepsAlive: true,
  }
  (commands, commandReceiver) := createChannel<WebSocketCommand>{
    capacity: options.commandCapacity,
    keepsAlive: true,
  }

  connection := WebSocketConnection {
    events,
    commands,
    options,
    eventSender,
    commandReceiver,
  }

  commandReceiver.onMessage((command: WebSocketCommand): void => handleWebSocketCommand(connection, command))
  commandReceiver.onClosed((): void => {
    ignored := connection.native.close(WEBSOCKET_CLOSE_NORMAL, "")
  })
  eventSender.onReady((): void => connection.native.resumeInboundReads())
  eventSender.onClosed((): void => {
    ignored := connection.native.close(WEBSOCKET_CLOSE_NORMAL, "")
  })

  return connection
}

export function upgradeNativeResponderToWebSocket(
  nativeResponder: NativeResponder,
  method: string,
  version: string,
  requestHeadersText: string,
  connection: WebSocketConnection,
): void {
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
    (event: NativeWebSocketEvent): int => {
      return emitNativeWebSocketEvent(connection, event)
    },
  )
}

export function failWebSocketConnection(
  connection: WebSocketConnection,
  error: ServerError,
): void {
  emitLocalWebSocketEvent(connection, WebSocketError {
    connection,
    error,
  })
  connection.commands.close()
  connection.events.close()
}

function handleWebSocketCommand(
  connection: WebSocketConnection,
  command: WebSocketCommand,
): void {
  textCommand := command as WebSocketSendText
  case textCommand {
    s: Success -> {
      reportCommandResult(connection, connection.native.sendText(s.value.text))
      return
    }
    _: Failure -> {}
  }

  binaryCommand := command as WebSocketSendBinary
  case binaryCommand {
    s: Success -> {
      reportCommandResult(connection, connection.native.sendBinary(s.value.bytes))
      return
    }
    _: Failure -> {}
  }

  pingCommand := command as WebSocketPing
  case pingCommand {
    _: Success -> {
      reportCommandResult(connection, connection.native.ping())
      return
    }
    _: Failure -> {}
  }

  closeCommand := command as WebSocketCloseCommand
  case closeCommand {
    s: Success -> {
      reportCommandResult(connection, connection.native.close(s.value.code, s.value.reason))
      return
    }
    _: Failure -> {}
  }
}

function reportCommandResult(
  connection: WebSocketConnection,
  result: Result<void, string>,
): void {
  mapped := mapNativeVoid(result)
  case mapped {
    _: Success -> {}
    f: Failure -> {
      emitLocalWebSocketEvent(connection, WebSocketError {
        connection,
        error: f.error,
      })
    }
  }
}

function emitLocalWebSocketEvent(
  connection: WebSocketConnection,
  event: WebSocketEvent,
): void {
  ignored := connection.eventSender.send(event)
}

function emitNativeWebSocketEvent(
  connection: WebSocketConnection,
  event: NativeWebSocketEvent,
): int {
  publicEvent := nativeWebSocketEventToPublic(connection, event)
  sent := connection.eventSender.send(publicEvent)
  code := channelSendResultToNativeCode(sent)

  if event.kind() == 4 || event.kind() == 5 {
    connection.commands.close()
    connection.events.close()
  }

  return code
}

function channelSendResultToNativeCode(
  sent: Result<Backpressure, SendError>,
): int {
  return case sent {
    s: Success -> case s.value {
      Backpressure.None -> 0,
      Backpressure.High -> 1,
    },
    f: Failure -> case f.error {
      SendError.Full -> 2,
      SendError.Closed -> 3,
    },
  }
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
