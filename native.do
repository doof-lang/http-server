import { BlobBuilder } from "std/blob"

export import class NativeExchange from "./native_http_server.hpp" as doof_http_server::NativeExchange {
  isolated method(): string
  isolated target(): string
  isolated version(): string
  isolated headersText(): string
  isolated body(): readonly byte[]
  isolated keepAlive(): bool
  isolated responder(): NativeResponder
}

export import class NativeResponder from "./native_http_server.hpp" as doof_http_server::NativeResponder {
  isolated respond(responseText: string, body: readonly byte[], keepAlive: bool): Result<void, string>
  isolated beginStreamResponse(responseText: string, keepAlive: bool): Result<void, string>
  isolated writeStreamBytes(bytes: readonly byte[]): Result<void, string>
  isolated endStreamResponse(bytes: readonly byte[]): Result<void, string>
  isolated upgradeToWebSocket(
    websocket: NativeWebSocketConnection,
    responseText: string,
  ): void
}

export import class NativeWebSocketEvent from "./native_http_server.hpp" as doof_http_server::NativeWebSocketEvent {
  isolated kind(): int
  isolated text(): string
  isolated bytes(): readonly byte[]
  isolated code(): int
  isolated reason(): string
  isolated wasClean(): bool
  isolated error(): string
}

export import class NativeWebSocketConnection from "./native_http_server.hpp" as doof_http_server::NativeWebSocketConnection {
  isolated static constructor(): NativeWebSocketConnection

  isolated sendRaw(
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

  isolated close(code: int, reason: string): Result<void, string>
  isolated resumeInboundReads(): void
  isolated state(): int
}

export import class NativeHttpServer from "./native_http_server.hpp" as doof_http_server::NativeHttpServer {
  isolated static listen(
    host: string,
    port: int,
    maxBodyBytes: long,
    idleTimeoutMillis: int,
    responseTimeoutMillis: int,
    maxRequestsPerConnection: int,
    onRequest: (exchange: NativeExchange): int,
  ): Result<NativeHttpServer, string>

  isolated host(): string
  isolated port(): int
  isolated close(): Result<void, string>
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
