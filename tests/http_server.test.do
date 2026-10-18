import { Assert } from "std/assert"
import { BlobBuilder } from "std/blob"
import { MainEventLoopHold, retainMainEventLoop, runMainEventLoop } from "std/event"
import { HttpHeader } from "std/http"

import {
  Request,
  RequestHandler,
  Response,
  ResponseCompression,
  Server,
  ServerOptions,
  WebSocketClose,
  WebSocketError,
  WebSocketEvent,
  WebSocketBinary,
  WebSocketOpen,
  WebSocketOptions,
  WebSocketText,
  WebSocketWritable,
  createWebSocketConnection,
  requestHandler,
} from "../index"

import class NativeHttpTestRequest from "../native_http_server_test_support.hpp" as doof_http_server_test::NativeHttpTestRequest {
  isolated static start(host: string, port: int, requestText: string): NativeHttpTestRequest
  isolated wait(): string
  isolated waitBytes(): readonly byte[]
}

import class NativeHttpSlowTestRequest from "../native_http_server_test_support.hpp" as doof_http_server_test::NativeHttpSlowTestRequest {
  isolated static start(host: string, port: int, firstChunk: string, secondChunk: string, delayMillis: int): NativeHttpSlowTestRequest
  isolated wait(): string
}

import class NativeHttpRequestParserFuzz from "../native_http_server_test_support.hpp" as doof_http_server_test::NativeHttpRequestParserFuzz {
  isolated static parse(requestText: string, maxBodyBytes: long): string
}

import class NativeWebSocketTestClient from "../native_http_server_test_support.hpp" as doof_http_server_test::NativeWebSocketTestClient {
  isolated static startExchangeText(host: string, port: int, requestText: string, text: string): NativeWebSocketTestClient
  isolated static startExchangeThreeTexts(host: string, port: int, requestText: string, first: string, second: string, third: string): NativeWebSocketTestClient
  isolated static startHandshakeOnly(host: string, port: int, requestText: string): NativeWebSocketTestClient
  isolated wait(): string
}

// Answers on its own actor so pool tests can see which handler served a request.
class PoolTestWorker {
  readonly id: int

  handler(): RequestHandler {
    return requestHandler((request: Request): none => {
      request.respond(Response.text(200, "worker ${this.id} ${request.path}\n")) else {}
    })
  }
}

class DispatchState {
  let method: string = ""
  let target: string = ""
  let path: string = ""
  let query: string = ""
  let host: string = ""
  let body: string = ""
}

class OneShotState {
  let secondKind: string = ""
}

class KeepAliveState {
  let count: int = 0
  let firstPath: string = ""
  let secondPath: string = ""
}

class SingleResponseState {
  let count: int = 0
  let secondKind: string = ""
}

class WebSocketTestState {
  let openCount: int = 0
  let textCount: int = 0
  let text: string = ""
  let closeCode: int = 0
  let errorKind: string = ""
  let errorMessage: string = ""
  let upgradeAttempt: bool = false
}

class ParserCase {
  let name: string = ""
  let requestText: string = ""
  let expectedPrefix: string = ""
}

class TestByteStream implements Stream<readonly byte[]> {
  chunks: string[]
  let index: int = 0
  let currentValue: readonly byte[] = []

  next(): bool {
    if this.index >= this.chunks.length {
      return false
    }
    this.currentValue = encodeTestText(this.chunks[this.index])
    this.index += 1
    return true
  }

  value(): readonly byte[] => this.currentValue
}

function encodeTestText(text: string): readonly byte[] {
  builder := BlobBuilder()
  builder.writeString(text)
  return builder.build()
}


function handleDispatch(
  state: DispatchState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  state.method = request.method
  state.target = request.target
  state.path = request.path
  state.query = request.queryString
  state.host = request.header("host") ?? ""
  state.body = request.getText()

  request.respond(Response.text(201, "created\n"))!
  hold.release()
}

function handleOneShot(
  state: OneShotState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  request.respond(Response.empty())!
  second := request.respond(Response.text(200, "too late"))
  case second {
    _: Success -> Assert.fail("expected second response to fail")
    f: Failure -> {
      state.secondKind = f.error.kind
    }
  }
  hold.release()
}

function handleKeepAlive(
  state: KeepAliveState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  state.count += 1
  if state.count == 1 {
    state.firstPath = request.path
    request.respond(Response.text(200, "first\n"))!
    return
  }

  state.secondPath = request.path
  request.respond(Response.text(200, "second\n"))!
  hold.release()
}

function handleSingleResponse(
  state: SingleResponseState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  state.count += 1
  request.respond(Response.text(200, "ok\n"))!
  hold.release()
}

function buildCompressionPayload(): readonly byte[] {
  builder := BlobBuilder()
  builder.writeString("compress me\n")
  builder.writeString("compress me\n")
  builder.writeString("compress me\n")
  return builder.build()
}

function handleGzipResponse(
  hold: MainEventLoopHold,
  request: Request,
): none {
  request.respond(Response {
    status: 200,
    headers: readonly [HttpHeader {
      name: "Content-Type",
      value: "text/plain; charset=utf-8",
    }],
    body: buildCompressionPayload(),
    compression: ResponseCompression.Compress,
  })!
  hold.release()
}

function handleDefaultTextResponse(
  hold: MainEventLoopHold,
  request: Request,
): none {
  request.respond(Response.text(
    200,
    "default compression\nactual response\n",
  ))!
  hold.release()
}

function handleStreamResponse(
  hold: MainEventLoopHold,
  request: Request,
): none {
  request.respond(Response.stream(
    200,
    TestByteStream {
      chunks: [
        "hello",
        "",
        "world",
      ],
    },
    readonly [HttpHeader {
      name: "Content-Type",
      value: "text/plain; charset=utf-8",
    }],
    ResponseCompression.None,
  ))!
  hold.release()
}

function handleStreamCloseResponse(
  hold: MainEventLoopHold,
  request: Request,
): none {
  request.respond(Response.stream(
    200,
    TestByteStream {
      chunks: [
        "bye",
      ],
    },
    readonly [HttpHeader {
      name: "Connection",
      value: "close",
    }],
    ResponseCompression.None,
  ))!
  hold.release()
}

function handleKeepAliveStream(
  state: KeepAliveState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  state.count += 1
  if state.count == 1 {
    state.firstPath = request.path
    request.respond(Response.stream(
      200,
      TestByteStream {
        chunks: [
          "first",
          " response\n",
        ],
      },
      readonly [],
      ResponseCompression.None,
    ))!
    return
  }

  state.secondPath = request.path
  request.respond(Response.text(200, "second\n"))!
  hold.release()
}

function handleStreamOneShot(
  state: SingleResponseState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  state.count += 1
  request.respond(Response.stream(
    200,
    TestByteStream {
      chunks: [
        "done",
      ],
    },
    readonly [],
    ResponseCompression.None,
  ))!
  second := request.respond(Response.text(200, "too late"))
  case second {
    _: Success -> Assert.fail("expected second response to fail")
    f: Failure -> {
      state.secondKind = f.error.kind
    }
  }
  hold.release()
}

function handleGzipStreamResponse(
  hold: MainEventLoopHold,
  request: Request,
): none {
  request.respond(Response.stream(
    200,
    TestByteStream {
      chunks: [
        "compress me\n",
        "compress me\n",
        "compress me\n",
      ],
    },
    readonly [HttpHeader {
      name: "Content-Type",
      value: "text/plain; charset=utf-8",
    }],
    ResponseCompression.Compress,
  ))!
  hold.release()
}

function handleEncodedStreamResponse(
  hold: MainEventLoopHold,
  request: Request,
): none {
  request.respond(Response.stream(
    200,
    TestByteStream {
      chunks: [
        "already encoded\n",
      ],
    },
    readonly [
      HttpHeader {
        name: "Content-Type",
        value: "text/plain; charset=utf-8",
      },
      HttpHeader {
        name: "Content-Encoding",
        value: "identity",
      },
    ],
    ResponseCompression.Compress,
  ))!
  hold.release()
}

function handleWithoutResponse(
  state: SingleResponseState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  state.count += 1
  hold.release()
}

function handleWebSocketEventAny(
  state: WebSocketTestState,
  event: WebSocketOpen | WebSocketText | WebSocketBinary | WebSocketWritable | WebSocketClose | WebSocketError,
): none {
  opened := event as WebSocketOpen
  case opened {
    _: Success -> {
      state.openCount += 1
      return
    }
    _: Failure -> {}
  }

  textEvent := event as WebSocketText
  case textEvent {
    textSuccess: Success -> {
      state.textCount += 1
      state.text = textSuccess.value.text
      textSuccess.value.connection.sendText("echo:" + textSuccess.value.text)!
      return
    }
    _: Failure -> {}
  }

  closeEvent := event as WebSocketClose
  case closeEvent {
    closeSuccess: Success -> {
      state.closeCode = closeSuccess.value.code
      return
    }
    _: Failure -> {}
  }

  errorEvent := event as WebSocketError
  case errorEvent {
    errorSuccess: Success -> {
      state.errorKind = errorSuccess.value.error.kind
      state.errorMessage = errorSuccess.value.error.message
      return
    }
    _: Failure -> {}
  }
}

function handleWebSocketUpgrade(
  state: WebSocketTestState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  state.upgradeAttempt = request.isWebSocketUpgrade()
  connection := createWebSocketConnection(
    (event: WebSocketEvent): none => handleWebSocketEventAny(state, event),
  )
  request.upgradeToWebSocket(connection)
  hold.release()
}

function handleBackpressuredWebSocketUpgrade(
  state: WebSocketTestState,
  hold: MainEventLoopHold,
  request: Request,
): none {
  state.upgradeAttempt = request.isWebSocketUpgrade()
  connection := createWebSocketConnection(
    (event: WebSocketEvent): none => handleWebSocketEventAny(state, event),
    WebSocketOptions { eventCapacity: 4 },
  )
  request.upgradeToWebSocket(connection)
  hold.release()
}

function assertRequestRejectedBeforeDispatch(requestText: string, statusLine: string): none {
  state := SingleResponseState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleSingleResponse(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    requestText,
  )

  response := client.wait()
  server.close()!

  Assert.equal(state.count, 0)
  Assert.isTrue(response.contains(statusLine))
  Assert.isTrue(response.contains("Connection: close"))
}

function assertParserCases(cases: ParserCase[]): none {
  for entry of cases {
    actual := NativeHttpRequestParserFuzz.parse(entry.requestText, 8L)
    Assert.isTrue(
      actual.startsWith(entry.expectedPrefix),
      "${entry.name}: expected ${entry.expectedPrefix}, got ${actual}",
    )
  }
}

function firstChunkPayloadOffset(responseBytes: readonly byte[], bodyStart: int): int {
  let cursor = bodyStart
  while cursor + 1 < responseBytes.length {
    if responseBytes[cursor] == byte(13) && responseBytes[cursor + 1] == byte(10) {
      return cursor + 2
    }
    cursor += 1
  }
  return -1
}

export function testServerDispatchesRequestsToHandler(): none {
  state := DispatchState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleDispatch(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "POST /items?q=doof HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  println("chunked dispatch state method=${state.method} body=${state.body} response=${response}")
  Assert.equal(state.method, "POST")
  Assert.equal(state.target, "/items?q=doof")
  Assert.equal(state.path, "/items")
  Assert.equal(state.query, "q=doof")
  Assert.equal(state.host, "example.test")
  Assert.equal(state.body, "hello")
  Assert.isTrue(response.contains("HTTP/1.1 201 Created"))
  Assert.isTrue(response.contains("Content-Type: text/plain; charset=utf-8"))
  Assert.isTrue(response.contains("created\n"))
}

export function testRequestResponderIsOneShot(): none {
  state := OneShotState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleOneShot(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.equal(state.secondKind, "already-responded")
  Assert.isTrue(response.contains("HTTP/1.1 204 No Content"))
}

export function testResponseConveniencesPreserveExplicitContentType(): none {
  response := Response.text(
    200,
    "hello",
    readonly [HttpHeader {
      name: "Content-Type",
      value: "text/custom",
    }],
  )

  Assert.equal(response.headers.length, 1)
  Assert.equal(response.headers[0].value, "text/custom")
}

export function testResponseGzipCompressionNegotiatesAcceptEncoding(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleGzipResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: br, gzip\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  responseBytes := client.waitBytes()
  server.close()!

  bodyStart := response.indexOf("\r\n\r\n") + 4
  Assert.isTrue(response.contains("HTTP/1.1 200 OK"), response)
  Assert.isTrue(response.contains("Content-Encoding: gzip"), response)
  Assert.isTrue(response.contains("Vary: Accept-Encoding"), response)
  Assert.equal(responseBytes[bodyStart], byte(31))
  Assert.equal(responseBytes[bodyStart + 1], byte(139))
  Assert.equal(responseBytes[bodyStart + 2], byte(8))
}

export function testResponseZstdCompressionNegotiatesAcceptEncoding(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleGzipResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: zstd, gzip\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  responseBytes := client.waitBytes()
  server.close()!

  bodyStart := response.indexOf("\r\n\r\n") + 4
  Assert.isTrue(response.contains("HTTP/1.1 200 OK"), response)
  Assert.isTrue(response.contains("Content-Encoding: zstd"), response)
  Assert.isTrue(response.contains("Vary: Accept-Encoding"), response)
  Assert.equal(responseBytes[bodyStart], byte(40))
  Assert.equal(responseBytes[bodyStart + 1], byte(181))
  Assert.equal(responseBytes[bodyStart + 2], byte(47))
  Assert.equal(responseBytes[bodyStart + 3], byte(253))
}

export function testDefaultResponseCompressionUsesTextPolicy(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleDefaultTextResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.isTrue(response.contains("Content-Encoding: gzip"), response)
  Assert.isTrue(response.contains("Vary: Accept-Encoding"), response)
  Assert.isFalse(response.contains("default compression\nactual response\n"), response)
}

export function testResponseCompressionSkipsWhenClientDoesNotAcceptGzip(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleGzipResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.isFalse(response.contains("Content-Encoding: gzip"), response)
  Assert.isTrue(response.contains("compress me\ncompress me\ncompress me\n"), response)
}

export function testStreamedResponseUsesChunkedTransferEncoding(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleStreamResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.isTrue(response.contains("HTTP/1.1 200 OK"), response)
  Assert.isTrue(response.contains("Transfer-Encoding: chunked"), response)
  Assert.isFalse(response.contains("Content-Length:"), response)
  Assert.isTrue(response.contains("\r\n5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n"), response)
}

export function testStreamedResponseConnectionCloseClosesAfterFinalChunk(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleStreamCloseResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.isTrue(response.contains("Connection: close"), response)
  Assert.isTrue(response.contains("\r\n3\r\nbye\r\n0\r\n\r\n"), response)
}

export function testStreamedKeepAliveResponseAllowsFollowingRequestAfterFinalChunk(): none {
  state := KeepAliveState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleKeepAliveStream(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET /first HTTP/1.1\r\nHost: example.test\r\n\r\nGET /second HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.equal(state.count, 2)
  Assert.equal(state.firstPath, "/first")
  Assert.equal(state.secondPath, "/second")
  Assert.isTrue(response.contains("Transfer-Encoding: chunked"), response)
  Assert.isTrue(response.contains("\r\n5\r\nfirst\r\na\r\n response\n\r\n0\r\n\r\nHTTP/1.1 200 OK"), response)
  Assert.isTrue(response.contains("second\n"), response)
}

export function testStreamedResponseResponderIsOneShot(): none {
  state := SingleResponseState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleStreamOneShot(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.equal(state.secondKind, "already-responded")
  Assert.isTrue(response.contains("\r\n4\r\ndone\r\n0\r\n\r\n"), response)
}

export function testStreamedGzipResponseNegotiatesAcceptEncoding(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleGzipStreamResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  responseBytes := client.waitBytes()
  server.close()!

  bodyStart := response.indexOf("\r\n\r\n") + 4
  payloadStart := firstChunkPayloadOffset(responseBytes, bodyStart)
  Assert.isTrue(response.contains("Transfer-Encoding: chunked"), response)
  Assert.isTrue(response.contains("Content-Encoding: gzip"), response)
  Assert.isTrue(response.contains("Vary: Accept-Encoding"), response)
  Assert.isTrue(payloadStart >= 0, "expected chunk payload offset")
  Assert.equal(responseBytes[payloadStart], byte(31))
  Assert.equal(responseBytes[payloadStart + 1], byte(139))
  Assert.equal(responseBytes[payloadStart + 2], byte(8))
}

export function testStreamedZstdResponseNegotiatesAcceptEncoding(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleGzipStreamResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: zstd, gzip\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  responseBytes := client.waitBytes()
  server.close()!

  bodyStart := response.indexOf("\r\n\r\n") + 4
  payloadStart := firstChunkPayloadOffset(responseBytes, bodyStart)
  Assert.isTrue(response.contains("Transfer-Encoding: chunked"), response)
  Assert.isTrue(response.contains("Content-Encoding: zstd"), response)
  Assert.isTrue(response.contains("Vary: Accept-Encoding"), response)
  Assert.isTrue(payloadStart >= 0, "expected chunk payload offset")
  Assert.equal(responseBytes[payloadStart], byte(40))
  Assert.equal(responseBytes[payloadStart + 1], byte(181))
  Assert.equal(responseBytes[payloadStart + 2], byte(47))
  Assert.equal(responseBytes[payloadStart + 3], byte(253))
}

export function testStreamedGzipResponseSkipsWhenClientDoesNotAcceptGzip(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleGzipStreamResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.isFalse(response.contains("Content-Encoding: gzip"), response)
  Assert.isTrue(response.contains("compress me\n"), response)
}

export function testStreamedGzipResponseSkipsWhenContentEncodingIsPresent(): none {
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleEncodedStreamResponse(hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.isTrue(response.contains("Content-Encoding: identity"), response)
  Assert.isFalse(response.contains("Content-Encoding: gzip"), response)
  Assert.isTrue(response.contains("already encoded\n"), response)
}

export function testWebSocketUpgradeDispatchesTextAndEchoesResponse(): none {
  state := WebSocketTestState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleWebSocketUpgrade(state, hold, request),
  }!

  client := NativeWebSocketTestClient.startExchangeText(
    server.host,
    server.port,
    "GET /socket HTTP/1.1\r\nHost: example.test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
    "hello",
  )

  runMainEventLoop()
  clientResponse := client.wait()
  server.close()!

  Assert.equal(state.openCount, 1, "openCount ${state.errorKind}:${state.errorMessage} ${clientResponse}")
  Assert.isTrue(state.upgradeAttempt)
  Assert.equal(state.text, "hello", "text")
  Assert.isTrue(clientResponse.contains("HTTP/1.1 101 Switching Protocols"), clientResponse)
  Assert.isTrue(clientResponse.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), clientResponse)
  Assert.isTrue(clientResponse.contains("frame|1|echo:hello"), clientResponse)
}

export function testWebSocketInboundBackpressurePausesAndResumesSocketReads(): none {
  state := WebSocketTestState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleBackpressuredWebSocketUpgrade(state, hold, request),
  }!

  client := NativeWebSocketTestClient.startExchangeThreeTexts(
    server.host,
    server.port,
    "GET /socket HTTP/1.1\r\nHost: example.test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
    "one",
    "two",
    "three",
  )

  runMainEventLoop()
  clientResponse := client.wait()
  server.close()!

  Assert.equal(state.openCount, 1, clientResponse)
  Assert.equal(state.textCount, 3, clientResponse)
  Assert.equal(state.text, "three", clientResponse)
  Assert.isTrue(clientResponse.contains("frame|1|echo:one"), clientResponse)
  Assert.isTrue(clientResponse.contains("frame|1|echo:two"), clientResponse)
  Assert.isTrue(clientResponse.contains("frame|1|echo:three"), clientResponse)
}

export function testInvalidWebSocketHandshakeReportsConnectionError(): none {
  state := WebSocketTestState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleWebSocketUpgrade(state, hold, request),
  }!

  client := NativeWebSocketTestClient.startHandshakeOnly(
    server.host,
    server.port,
    "GET /socket HTTP/1.1\r\nHost: example.test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 12\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
  )

  runMainEventLoop()
  clientResponse := client.wait()
  server.close()!

  Assert.equal(state.errorKind, "bad-websocket-handshake")
  Assert.isTrue(state.upgradeAttempt)
  Assert.isTrue(clientResponse.contains("HTTP/1.1 400 Bad Request"))
}

export function testHttp11ConnectionCanServeSequentialRequests(): none {
  state := KeepAliveState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleKeepAlive(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET /first HTTP/1.1\r\nHost: example.test\r\n\r\nGET /second HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.equal(state.count, 2)
  Assert.equal(state.firstPath, "/first")
  Assert.equal(state.secondPath, "/second")
  Assert.equal(response.split("HTTP/1.1 200 OK").length, 3)
  Assert.isTrue(response.contains("Connection: keep-alive"))
  Assert.isTrue(response.contains("Connection: close"))
  Assert.isTrue(response.contains("first\n"))
  Assert.isTrue(response.contains("second\n"))
}

export function testIdleKeepAliveConnectionExpires(): none {
  state := SingleResponseState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, idleTimeoutMillis: 20, keepsAlive: false },
    onRequest: (request: Request): none => handleSingleResponse(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.equal(state.count, 1)
  Assert.isTrue(response.contains("HTTP/1.1 200 OK"))
  Assert.isTrue(response.contains("Connection: keep-alive"))
}

export function testConnectionRequestLimitClosesAfterConfiguredCount(): none {
  state := SingleResponseState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, maxRequestsPerConnection: 1, keepsAlive: false },
    onRequest: (request: Request): none => handleSingleResponse(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET /first HTTP/1.1\r\nHost: example.test\r\n\r\nGET /second HTTP/1.1\r\nHost: example.test\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.equal(state.count, 1)
  Assert.equal(response.split("HTTP/1.1 200 OK").length, 2)
  Assert.isTrue(response.contains("Connection: close"))
}

export function testHandlerThatNeverRespondsTimesOutRequest(): none {
  state := SingleResponseState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, responseTimeoutMillis: 20, keepsAlive: false },
    onRequest: (request: Request): none => handleWithoutResponse(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.equal(state.count, 1)
  Assert.isTrue(response.contains("HTTP/1.1 504 Gateway Timeout"))
  Assert.isTrue(response.contains("Connection: close"))
}

export function testSlowPartialHeadersExpireWithoutDispatch(): none {
  state := SingleResponseState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, idleTimeoutMillis: 20, keepsAlive: false },
    onRequest: (request: Request): none => handleSingleResponse(state, hold, request),
  }!

  client := NativeHttpSlowTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test",
    "",
    80,
  )

  response := client.wait()
  server.close()!

  Assert.equal(state.count, 0)
  Assert.equal(response, "")
}

export function testChunkedRequestBodyIsDispatched(): none {
  state := DispatchState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleDispatch(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  server.close()!

  Assert.equal(state.method, "POST")
  Assert.equal(state.body, "hello")
  Assert.isTrue(response.contains("HTTP/1.1 201 Created"))
}

export function testMalformedParserInputsAreRejectedBeforeDispatch(): none {
  assertRequestRejectedBeforeDispatch(
    "G ET / HTTP/1.1\r\nHost: example.test\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
  assertRequestRejectedBeforeDispatch(
    "GET /bad target HTTP/1.1\r\nHost: example.test\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
  assertRequestRejectedBeforeDispatch(
    "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: abc\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
  assertRequestRejectedBeforeDispatch(
    "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na",
    "HTTP/1.1 400 Bad Request",
  )
}

export function testHttp11RequestRequiresHostHeader(): none {
  assertRequestRejectedBeforeDispatch(
    "GET / HTTP/1.1\r\nConnection: close\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
}

export function testMalformedRequestLineIsRejected(): none {
  assertRequestRejectedBeforeDispatch(
    "GET / HTTP/1.1 extra\r\nHost: example.test\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
}

export function testInvalidHeaderNameIsRejected(): none {
  assertRequestRejectedBeforeDispatch(
    "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length : 5\r\n\r\nhello",
    "HTTP/1.1 400 Bad Request",
  )
}

export function testUnsupportedHttpVersionIsRejected(): none {
  assertRequestRejectedBeforeDispatch(
    "GET / HTTP/1.2\r\nHost: example.test\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
}

export function testParserFuzzCorpusForLengthTransferAndWhitespaceCombinations(): none {
  assertParserCases([
    ParserCase {
      name: "trimmed content length",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length:\t5 \r\n\r\nhelloGET /next HTTP/1.1\r\nHost: example.test\r\n\r\n",
      expectedPrefix: "complete|POST|/|HTTP/1.1|keep-alive|5|42|",
    },
    ParserCase {
      name: "content length too small leaves pipelined bytes",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 4\r\n\r\nhello",
      expectedPrefix: "complete|POST|/|HTTP/1.1|keep-alive|4|1|",
    },
    ParserCase {
      name: "content length too large waits for body",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 6\r\n\r\nhello",
      expectedPrefix: "need-more",
    },
    ParserCase {
      name: "body size limit",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 9\r\n\r\nhello",
      expectedPrefix: "error|body-too-large|",
    },
    ParserCase {
      name: "duplicate content length",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "signed content length",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: +5\r\n\r\nhello",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "overflow content length",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 9223372036854775808\r\n\r\nhello",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "chunked transfer encoding",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
      expectedPrefix: "complete|POST|/|HTTP/1.1|keep-alive|5|0|",
    },
    ParserCase {
      name: "transfer encoding with content length",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 5\r\nTransfer-Encoding: gzip\r\n\r\nhello",
      expectedPrefix: "error|unsupported-transfer-encoding|",
    },
    ParserCase {
      name: "chunked transfer encoding with content length",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "chunked transfer encoding with split body",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhell",
      expectedPrefix: "need-more",
    },
    ParserCase {
      name: "chunked transfer encoding over body limit",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\n9\r\nhello!!!!\r\n0\r\n\r\n",
      expectedPrefix: "error|body-too-large|",
    },
    ParserCase {
      name: "malformed chunk size",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: chunked\r\n\r\nz\r\nhello\r\n0\r\n\r\n",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "identity transfer encoding",
      requestText: "POST / HTTP/1.1\r\nHost: example.test\r\nTransfer-Encoding: identity\r\nContent-Length: 5\r\n\r\nhello",
      expectedPrefix: "complete|POST|/|HTTP/1.1|keep-alive|5|0|",
    },
    ParserCase {
      name: "connection token whitespace",
      requestText: "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: keep-alive, close \r\n\r\n",
      expectedPrefix: "complete|GET|/|HTTP/1.1|close|0|0|",
    },
  ])
}

export function testParserFuzzCorpusForHeaderShapeAndObsFoldLikeInputs(): none {
  assertParserCases([
    ParserCase {
      name: "header name with whitespace",
      requestText: "GET / HTTP/1.1\r\nHost: example.test\r\nBad Header: value\r\n\r\n",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "header value with bare carriage return",
      requestText: "GET / HTTP/1.1\r\nHost: example.test\r\nX-Test: one\rtwo\r\n\r\n",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "obs fold continuation",
      requestText: "GET / HTTP/1.1\r\nHost: example.test\r\nX-Test: one\r\n two\r\n\r\n",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "obs fold with tab",
      requestText: "GET / HTTP/1.1\r\nHost: example.test\r\nX-Test: one\r\n\ttwo\r\n\r\n",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "repeated ordinary header",
      requestText: "GET / HTTP/1.1\r\nHost: example.test\r\nAccept: text/plain\r\nAccept: application/json\r\n\r\n",
      expectedPrefix: "complete|GET|/|HTTP/1.1|keep-alive|0|0|",
    },
    ParserCase {
      name: "duplicate host",
      requestText: "GET / HTTP/1.1\r\nHost: example.test\r\nHost: other.test\r\n\r\n",
      expectedPrefix: "error|malformed-request|",
    },
    ParserCase {
      name: "http10 without host closes by default",
      requestText: "GET / HTTP/1.0\r\n\r\n",
      expectedPrefix: "complete|GET|/|HTTP/1.0|close|0|0|",
    },
    ParserCase {
      name: "http10 keep alive token",
      requestText: "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
      expectedPrefix: "complete|GET|/|HTTP/1.0|keep-alive|0|0|",
    },
  ])
}

export function testRejectedRequestClosesBeforePipelinedBytesAreDispatched(): none {
  state := SingleResponseState()
  hold := retainMainEventLoop()

  server := Server.listen{
    options: ServerOptions { port: 0, keepsAlive: false },
    onRequest: (request: Request): none => handleSingleResponse(state, hold, request),
  }!

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "POST /bad HTTP/1.1\r\nHost: example.test\r\nContent-Length: nope\r\n\r\nGET /ok HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  response := client.wait()
  server.close()!

  Assert.equal(state.count, 0)
  Assert.isTrue(response.contains("HTTP/1.1 400 Bad Request"))
  Assert.equal(response.split("HTTP/1.1").length, 2)
  Assert.isTrue(response.contains("Connection: close"))
}

readonly POOL_REQUESTS = "GET /a HTTP/1.1\r\nHost: example.test\r\n\r\nGET /b HTTP/1.1\r\nHost: example.test\r\n\r\nGET /c HTTP/1.1\r\nHost: example.test\r\n\r\nGET /d HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"

export function testListenPoolSpreadsRequestsAcrossHandlerActors(): none {
  first := Actor<PoolTestWorker>(0)
  second := Actor<PoolTestWorker>(1)
  server := Server.listenPool{
    options: ServerOptions { port: 0, keepsAlive: false },
    handlers: [first.handler(), second.handler()],
  }!

  response := NativeHttpTestRequest.start(server.host, server.port, POOL_REQUESTS).wait()
  server.close()!

  Assert.equal(response.split("HTTP/1.1 200 OK").length, 5)
  Assert.isTrue(response.contains("worker 0 "))
  Assert.isTrue(response.contains("worker 1 "))
  Assert.equal(server.shedCount(), 0L)
}

export function testSetHandlersRoutesNewRequestsToReplacementHandlers(): none {
  first := Actor<PoolTestWorker>(0)
  second := Actor<PoolTestWorker>(1)
  server := Server.listenPool{
    options: ServerOptions { port: 0, keepsAlive: false },
    handlers: [first.handler(), second.handler()],
  }!
  server.setHandlers([second.handler()])!

  response := NativeHttpTestRequest.start(server.host, server.port, POOL_REQUESTS).wait()
  server.close()!

  Assert.equal(response.split("worker 1 ").length, 5)
  Assert.isFalse(response.contains("worker 0 "))
}

export function testListenPoolRejectsEmptyHandlers(): none {
  empty: readonly RequestHandler[] := []
  case Server.listenPool{ options: ServerOptions { port: 0, keepsAlive: false }, handlers: empty } {
    _: Success -> Assert.fail("expected listenPool to reject an empty handler list"),
    f: Failure -> Assert.equal(f.error.kind, "config"),
  }
}
