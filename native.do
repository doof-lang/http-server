import { BlobBuilder } from "std/blob"

export import class NativeExchange from "./native_http_server.hpp" as doof_http_server::NativeExchange {
  method(): string
  target(): string
  version(): string
  headersText(): string
  body(): readonly byte[]
  keepAlive(): bool
  responder(): NativeResponder
}

export import class NativeResponder from "./native_http_server.hpp" as doof_http_server::NativeResponder {
  respond(responseText: string, body: readonly byte[], keepAlive: bool): Result<void, string>
  beginStreamResponse(responseText: string, keepAlive: bool): Result<void, string>
  writeStreamBytes(bytes: readonly byte[]): Result<void, string>
  endStreamResponse(bytes: readonly byte[]): Result<void, string>
  upgradeToWebSocket(
    websocket: NativeWebSocketConnection,
    responseText: string,
    callback: (event: NativeWebSocketEvent): void,
  ): void
}

export import class NativeWebSocketEvent from "./native_http_server.hpp" as doof_http_server::NativeWebSocketEvent {
  kind(): int
  text(): string
  bytes(): readonly byte[]
  code(): int
  reason(): string
  wasClean(): bool
  error(): string
}

export import class NativeWebSocketConnection from "./native_http_server.hpp" as doof_http_server::NativeWebSocketConnection {
  static constructor(): NativeWebSocketConnection

  sendRaw(
    opcode: int,
    payload: readonly byte[],
    closeCode: int,
    closeReason: string,
  ): Result<void, string>

  sendText(text: string): Result<void, string> {
    return this.sendRaw(1, encodeText(text), 0, "")
  }

  sendBinary(bytes: readonly byte[]): Result<void, string> {
    return this.sendRaw(2, bytes, 0, "")
  }

  ping(): Result<void, string> {
    return this.sendRaw(9, emptyBytes(), 0, "")
  }

  close(code: int, reason: string): Result<void, string>
  state(): int
}

export import class NativeHttpServer from "./native_http_server.hpp" as doof_http_server::NativeHttpServer {
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

function encodeText(text: string): readonly byte[] {
  builder := BlobBuilder()
  builder.writeString(text)
  return builder.build()
}

function emptyBytes(): readonly byte[] {
  builder := BlobBuilder()
  return builder.build()
}
