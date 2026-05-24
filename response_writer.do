import { BlobBuilder } from "std/blob"
import { gzip, GzipStream } from "std/gzip"
import { HttpHeader } from "std/http"

import { ServerError, mapNativeVoid } from "./errors"
import { hasHeader, headerContainsTokenValue, headerValue, withHeader, withVaryAcceptEncoding } from "./headers"
import { NativeResponder } from "./native"
import { Response, ResponseCompression } from "./response"

export class ResponseRequestContext {
  readonly headers: readonly HttpHeader[]
  readonly keepAlive: bool
}

export function respondToNative(
  nativeResponder: NativeResponder,
  request: ResponseRequestContext,
  response: Response,
): Result<void, ServerError> {
  return case response.body {
    body: readonly byte[] -> respondWithBytes(nativeResponder, request, response, body),
    body: Stream<readonly byte[]> -> respondWithStream(nativeResponder, request, response, body),
  }
}

function acceptEncodingPartName(part: string): string {
  separator := part.indexOf(";")
  if separator < 0 {
    return part.trim().toLowerCase()
  }
  return part.substring(0, separator).trim().toLowerCase()
}

function acceptEncodingPartAllows(part: string): bool {
  parameters := part.split(";")
  for index of 1..<parameters.length {
    parameter := parameters[index].trim()
    separator := parameter.indexOf("=")
    if separator < 0 {
      continue
    }
    if parameter.substring(0, separator).trim().toLowerCase() != "q" {
      continue
    }
    quality := parameter.slice(separator + 1).trim()
    if quality == "0" || quality == "0.0" || quality == "0.00" || quality == "0.000" {
      return false
    }
  }
  return true
}

function requestAcceptsEncoding(request: ResponseRequestContext, encoding: string): bool {
  lowerEncoding := encoding.toLowerCase()
  let explicitRejected = false
  let wildcardAccepted = false
  for header of request.headers {
    if header.name.toLowerCase() != "accept-encoding" {
      continue
    }

    parts := header.value.split(",")
    for part of parts {
      name := acceptEncodingPartName(part)
      allowed := acceptEncodingPartAllows(part)
      if name == lowerEncoding {
        if allowed {
          return true
        }
        explicitRejected = true
      } else if name == "*" && allowed {
        wildcardAccepted = true
      }
    }
  }
  return !explicitRejected && wildcardAccepted
}

function isDefaultCompressible(headers: readonly HttpHeader[]): bool {
  contentType := headerValue(headers, "Content-Type") else {
    return false
  }
  lowerContentType := contentType.toLowerCase()
  return lowerContentType.startsWith("text/") ||
    lowerContentType.startsWith("application/json") ||
    lowerContentType.startsWith("application/javascript") ||
    lowerContentType.startsWith("application/xml") ||
    lowerContentType.startsWith("image/svg+xml")
}

function shouldCompressByteResponse(
  request: ResponseRequestContext,
  response: Response,
  body: readonly byte[],
): bool {
  if body.length == 0 {
    return false
  }
  if hasHeader(response.headers, "Content-Encoding") {
    return false
  }
  if !requestAcceptsEncoding(request, "gzip") {
    return false
  }

  return case response.compression {
    ResponseCompression.None -> false,
    ResponseCompression.Compress -> true,
    ResponseCompression.Default -> isDefaultCompressible(response.headers),
  }
}

function shouldCompressStreamResponse(request: ResponseRequestContext, response: Response): bool {
  if hasHeader(response.headers, "Content-Encoding") {
    return false
  }
  if !requestAcceptsEncoding(request, "gzip") {
    return false
  }

  return case response.compression {
    ResponseCompression.None -> false,
    ResponseCompression.Compress -> true,
    ResponseCompression.Default -> isDefaultCompressible(response.headers),
  }
}

class ByteResponse {
  readonly status: int
  readonly headers: readonly HttpHeader[]
  readonly body: readonly byte[]
}

class StreamResponse {
  readonly status: int
  readonly headers: readonly HttpHeader[]
  readonly body: Stream<readonly byte[]>
}

function byteResponseForRequest(
  request: ResponseRequestContext,
  response: Response,
  body: readonly byte[],
): ByteResponse {
  if !shouldCompressByteResponse(request, response, body) {
    return ByteResponse {
      status: response.status,
      headers: response.headers,
      body,
    }
  }

  headersWithEncoding := withHeader(response.headers, "Content-Encoding", "gzip")
  return ByteResponse {
    status: response.status,
    headers: withVaryAcceptEncoding(headersWithEncoding),
    body: gzip(body),
  }
}

function streamResponseForRequest(
  request: ResponseRequestContext,
  response: Response,
  body: Stream<readonly byte[]>,
): StreamResponse {
  if !shouldCompressStreamResponse(request, response) {
    return StreamResponse {
      status: response.status,
      headers: response.headers,
      body,
    }
  }

  headersWithEncoding := withHeader(response.headers, "Content-Encoding", "gzip")
  return StreamResponse {
    status: response.status,
    headers: withVaryAcceptEncoding(headersWithEncoding),
    body: GzipStream(body),
  }
}

function respondWithBytes(
  nativeResponder: NativeResponder,
  request: ResponseRequestContext,
  response: Response,
  body: readonly byte[],
): Result<void, ServerError> {
  finalResponse := byteResponseForRequest(request, response, body)
  keepAlive := responseKeepAlive(request, finalResponse.headers)
  return mapNativeVoid(nativeResponder.respond(
    fixedResponseHeadText(finalResponse.status, finalResponse.headers, finalResponse.body.length, keepAlive),
    finalResponse.body,
    keepAlive,
  ))
}

function respondWithStream(
  nativeResponder: NativeResponder,
  request: ResponseRequestContext,
  response: Response,
  body: Stream<readonly byte[]>,
): Result<void, ServerError> {
  finalResponse := streamResponseForRequest(request, response, body)
  keepAlive := responseKeepAlive(request, finalResponse.headers)

  started := mapNativeVoid(nativeResponder.beginStreamResponse(
    chunkedResponseHeadText(finalResponse.status, finalResponse.headers, keepAlive),
    keepAlive,
  ))
  case started {
    _: Success -> {}
    f: Failure -> return Failure {
      error: f.error
    }
  }

  for chunk of finalResponse.body {
    if chunk.length == 0 {
      continue
    }
    written := mapNativeVoid(nativeResponder.writeStreamBytes(chunkedResponseChunkBytes(chunk)))
    case written {
      _: Success -> {}
      f: Failure -> return Failure {
        error: f.error
      }
    }
  }

  return mapNativeVoid(nativeResponder.endStreamResponse(encodeText("0\r\n\r\n")))
}

function encodeText(text: string): readonly byte[] {
  builder := BlobBuilder()
  builder.writeString(text)
  return builder.build()
}

function chunkedResponseChunkBytes(chunk: readonly byte[]): readonly byte[] {
  builder := BlobBuilder()
  builder.writeString(hexInt(chunk.length))
  builder.writeString("\r\n")
  builder.writeBytes(chunk)
  builder.writeString("\r\n")
  return builder.build()
}

function hexInt(value: int): string {
  if value == 0 {
    return "0"
  }
  let remaining = value
  let text = ""
  while remaining > 0 {
    digit := remaining % 16
    text = hexDigit(digit) + text
    remaining = remaining \ 16
  }
  return text
}

function hexDigit(value: int): string {
  return case value {
    0 -> "0",
    1 -> "1",
    2 -> "2",
    3 -> "3",
    4 -> "4",
    5 -> "5",
    6 -> "6",
    7 -> "7",
    8 -> "8",
    9 -> "9",
    10 -> "a",
    11 -> "b",
    12 -> "c",
    13 -> "d",
    14 -> "e",
    _ -> "f",
  }
}

function responseKeepAlive(request: ResponseRequestContext, headers: readonly HttpHeader[]): bool {
  return request.keepAlive && !responseRequestsClose(headers)
}

function responseRequestsClose(headers: readonly HttpHeader[]): bool {
  value := headerValue(headers, "Connection") else {
    return false
  }
  return headerContainsTokenValue(value, "close")
}

function fixedResponseHeadText(
  status: int,
  headers: readonly HttpHeader[],
  bodySize: int,
  keepAlive: bool,
): string {
  return responseStatusLine(status) +
    normalizedFixedResponseHeaders(headers, bodySize, keepAlive) +
    "\r\n"
}

function chunkedResponseHeadText(
  status: int,
  headers: readonly HttpHeader[],
  keepAlive: bool,
): string {
  return responseStatusLine(status) +
    normalizedChunkedResponseHeaders(headers, keepAlive) +
    "\r\n"
}

function responseStatusLine(status: int): string {
  return "HTTP/1.1 ${status} ${statusText(status)}\r\n"
}

function normalizedFixedResponseHeaders(
  headers: readonly HttpHeader[],
  bodySize: int,
  keepAlive: bool,
): string {
  let text = ""
  for header of headers {
    lowerName := header.name.toLowerCase()
    if lowerName == "content-length" || lowerName == "connection" {
      continue
    }
    text += "${header.name}: ${header.value}\r\n"
  }
  text += "Content-Length: ${bodySize}\r\n"
  text += "Connection: ${if keepAlive then "keep-alive" else "close"}\r\n"
  return text
}

function normalizedChunkedResponseHeaders(
  headers: readonly HttpHeader[],
  keepAlive: bool,
): string {
  let text = ""
  for header of headers {
    lowerName := header.name.toLowerCase()
    if lowerName == "content-length" || lowerName == "connection" || lowerName == "transfer-encoding" {
      continue
    }
    text += "${header.name}: ${header.value}\r\n"
  }
  text += "Transfer-Encoding: chunked\r\n"
  text += "Connection: ${if keepAlive then "keep-alive" else "close"}\r\n"
  return text
}

function statusText(status: int): string {
  return case status {
    100 -> "Continue",
    101 -> "Switching Protocols",
    200 -> "OK",
    201 -> "Created",
    202 -> "Accepted",
    204 -> "No Content",
    301 -> "Moved Permanently",
    302 -> "Found",
    304 -> "Not Modified",
    307 -> "Temporary Redirect",
    308 -> "Permanent Redirect",
    400 -> "Bad Request",
    401 -> "Unauthorized",
    403 -> "Forbidden",
    404 -> "Not Found",
    405 -> "Method Not Allowed",
    408 -> "Request Timeout",
    409 -> "Conflict",
    410 -> "Gone",
    413 -> "Payload Too Large",
    415 -> "Unsupported Media Type",
    418 -> "I'm a teapot",
    429 -> "Too Many Requests",
    500 -> "Internal Server Error",
    501 -> "Not Implemented",
    502 -> "Bad Gateway",
    503 -> "Service Unavailable",
    504 -> "Gateway Timeout",
    _ -> "HTTP ${status}",
  }
}
