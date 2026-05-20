import { Assert } from "std/assert"
import { AsyncEventChannel, createMainAsyncEventChannel, runMainEventLoop } from "std/event"
import { HttpHeader } from "std/http"

import { Request, Response, Server, ServerOptions } from "../index"

import class NativeHttpTestRequest from "../native_http_server_test_support.hpp" as doof_http_server_test::NativeHttpTestRequest {
  static start(host: string, port: int, requestText: string): NativeHttpTestRequest
  wait(): string
}

class DispatchState {
  method: string = ""
  target: string = ""
  path: string = ""
  query: string = ""
  host: string = ""
  body: string = ""
}

class OneShotState {
  secondKind: string = ""
}

class KeepAliveState {
  count: int = 0
  firstPath: string = ""
  secondPath: string = ""
}

class SingleResponseState {
  count: int = 0
}

function handleDispatch(
  state: DispatchState,
  requestChannel: AsyncEventChannel<Request>,
  request: Request,
): void {
  state.method = request.method
  state.target = request.target
  state.path = request.path
  state.query = request.queryString
  state.host = request.header("host") ?? ""
  state.body = request.getText()

  try! request.respond(Response.text(201, "created\n"))
  try! requestChannel.close()
}

function handleOneShot(
  state: OneShotState,
  requestChannel: AsyncEventChannel<Request>,
  request: Request,
): void {
  try! request.respond(Response.empty())
  second := request.respond(Response.text(200, "too late"))
  case second {
    _: Success -> Assert.fail("expected second response to fail")
    f: Failure -> {
      state.secondKind = f.error.kind
    }
  }
  try! requestChannel.close()
}

function handleKeepAlive(
  state: KeepAliveState,
  requestChannel: AsyncEventChannel<Request>,
  request: Request,
): void {
  state.count += 1
  if state.count == 1 {
    state.firstPath = request.path
    try! request.respond(Response.text(200, "first\n"))
    return
  }

  state.secondPath = request.path
  try! request.respond(Response.text(200, "second\n"))
  try! requestChannel.close()
}

function handleSingleResponse(
  state: SingleResponseState,
  requestChannel: AsyncEventChannel<Request>,
  request: Request,
): void {
  state.count += 1
  try! request.respond(Response.text(200, "ok\n"))
  try! requestChannel.close()
}

function assertRequestRejectedBeforeDispatch(requestText: string, statusLine: string): void {
  state := SingleResponseState()
  let requestChannel: AsyncEventChannel<Request> | null = null

  requests := createMainAsyncEventChannel<Request>{
    handler: (request: Request): void => handleSingleResponse(state, requestChannel!, request),
    capacity: 1,
    keepsAlive: true,
  }
  requestChannel = requests

  server := try! Server.listen{
    options: ServerOptions { port: 0 },
    requests,
  }

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    requestText,
  )

  response := client.wait()
  try! server.close()

  Assert.equal(state.count, 0)
  Assert.isTrue(response.contains(statusLine))
  Assert.isTrue(response.contains("Connection: close"))
}

export function testServerDispatchesRequestsThroughAsyncEventChannel(): void {
  state := DispatchState()
  let requestChannel: AsyncEventChannel<Request> | null = null

  requests := createMainAsyncEventChannel<Request>{
    handler: (request: Request): void => handleDispatch(state, requestChannel!, request),
    capacity: 4,
    keepsAlive: true,
  }
  requestChannel = requests

  server := try! Server.listen{
    options: ServerOptions { port: 0 },
    requests,
  }

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "POST /items?q=doof HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello",
  )

  runMainEventLoop()
  response := client.wait()
  try! server.close()

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

export function testRequestResponderIsOneShot(): void {
  state := OneShotState()
  let requestChannel: AsyncEventChannel<Request> | null = null

  requests := createMainAsyncEventChannel<Request>{
    handler: (request: Request): void => handleOneShot(state, requestChannel!, request),
    capacity: 1,
    keepsAlive: true,
  }
  requestChannel = requests

  server := try! Server.listen{
    options: ServerOptions { port: 0 },
    requests,
  }

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  try! server.close()

  Assert.equal(state.secondKind, "already-responded")
  Assert.isTrue(response.contains("HTTP/1.1 204 No Content"))
}

export function testResponseConveniencesPreserveExplicitContentType(): void {
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

export function testHttp11ConnectionCanServeSequentialRequests(): void {
  state := KeepAliveState()
  let requestChannel: AsyncEventChannel<Request> | null = null

  requests := createMainAsyncEventChannel<Request>{
    handler: (request: Request): void => handleKeepAlive(state, requestChannel!, request),
    capacity: 4,
    keepsAlive: true,
  }
  requestChannel = requests

  server := try! Server.listen{
    options: ServerOptions { port: 0 },
    requests,
  }

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET /first HTTP/1.1\r\nHost: example.test\r\n\r\nGET /second HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  try! server.close()

  Assert.equal(state.count, 2)
  Assert.equal(state.firstPath, "/first")
  Assert.equal(state.secondPath, "/second")
  Assert.equal(response.split("HTTP/1.1 200 OK").length, 3)
  Assert.isTrue(response.contains("Connection: keep-alive"))
  Assert.isTrue(response.contains("Connection: close"))
  Assert.isTrue(response.contains("first\n"))
  Assert.isTrue(response.contains("second\n"))
}

export function testIdleKeepAliveConnectionExpires(): void {
  state := SingleResponseState()
  let requestChannel: AsyncEventChannel<Request> | null = null

  requests := createMainAsyncEventChannel<Request>{
    handler: (request: Request): void => handleSingleResponse(state, requestChannel!, request),
    capacity: 1,
    keepsAlive: true,
  }
  requestChannel = requests

  server := try! Server.listen{
    options: ServerOptions { port: 0, idleTimeoutMillis: 20 },
    requests,
  }

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  try! server.close()

  Assert.equal(state.count, 1)
  Assert.isTrue(response.contains("HTTP/1.1 200 OK"))
  Assert.isTrue(response.contains("Connection: keep-alive"))
}

export function testConnectionRequestLimitClosesAfterConfiguredCount(): void {
  state := SingleResponseState()
  let requestChannel: AsyncEventChannel<Request> | null = null

  requests := createMainAsyncEventChannel<Request>{
    handler: (request: Request): void => handleSingleResponse(state, requestChannel!, request),
    capacity: 2,
    keepsAlive: true,
  }
  requestChannel = requests

  server := try! Server.listen{
    options: ServerOptions { port: 0, maxRequestsPerConnection: 1 },
    requests,
  }

  client := NativeHttpTestRequest.start(
    server.host,
    server.port,
    "GET /first HTTP/1.1\r\nHost: example.test\r\n\r\nGET /second HTTP/1.1\r\nHost: example.test\r\n\r\n",
  )

  runMainEventLoop()
  response := client.wait()
  try! server.close()

  Assert.equal(state.count, 1)
  Assert.equal(response.split("HTTP/1.1 200 OK").length, 2)
  Assert.isTrue(response.contains("Connection: close"))
}

export function testHttp11RequestRequiresHostHeader(): void {
  assertRequestRejectedBeforeDispatch(
    "GET / HTTP/1.1\r\nConnection: close\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
}

export function testMalformedRequestLineIsRejected(): void {
  assertRequestRejectedBeforeDispatch(
    "GET / HTTP/1.1 extra\r\nHost: example.test\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
}

export function testInvalidHeaderNameIsRejected(): void {
  assertRequestRejectedBeforeDispatch(
    "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length : 5\r\n\r\nhello",
    "HTTP/1.1 400 Bad Request",
  )
}

export function testUnsupportedHttpVersionIsRejected(): void {
  assertRequestRejectedBeforeDispatch(
    "GET / HTTP/1.2\r\nHost: example.test\r\n\r\n",
    "HTTP/1.1 400 Bad Request",
  )
}
