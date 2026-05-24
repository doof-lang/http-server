import { BlobReader } from "std/blob"
import { HttpHeader } from "std/http"
import { parseJsonValue } from "std/json"

import { ServerError } from "./errors"
import { headersAreSafe, parseHeaders, renderHeaders } from "./headers"
import { NativeExchange, NativeResponder } from "./native"
import { Response } from "./response"
import { ResponseRequestContext, respondToNative } from "./response_writer"
import { WebSocketConnection, WebSocketError, upgradeNativeResponderToWebSocket } from "./websocket"

export class Request {
  readonly method: string
  readonly target: string
  readonly path: string
  readonly queryString: string
  readonly version: string
  readonly headers: readonly HttpHeader[]
  readonly body: readonly byte[]
  private readonly keepAlive: bool = false
  private readonly responder: NativeResponder | null = null

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

  isWebSocketUpgrade(): bool {
    upgrade := this.header("Upgrade") else {
      return false
    }
    return upgrade.trim().toLowerCase() == "websocket" && this.headerContainsToken("Connection", "upgrade")
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
    nativeResponder := this.responder else {
      return Failure {
        error: ServerError {
          kind: "missing-responder",
          message: "Request was not created by Server.listen and cannot send a response",
        }
      }
    }

    if !headersAreSafe(response.headers) {
      return Failure {
        error: ServerError {
          kind: "invalid-header",
          message: "Response headers cannot contain CR or LF characters",
        }
      }
    }

    return respondToNative(
      nativeResponder,
      ResponseRequestContext {
        headers: this.headers,
        keepAlive: this.keepAlive,
      },
      response,
    )
  }

  upgradeToWebSocket(connection: WebSocketConnection): void {
    nativeResponder := this.responder else {
      connection.handler(WebSocketError {
        connection,
        error: ServerError {
          kind: "missing-responder",
          message: "Request was not created by Server.listen and cannot be upgraded",
        },
      })
      return
    }

    upgradeNativeResponderToWebSocket(
      nativeResponder,
      this.method,
      this.version,
      renderHeaders(this.headers),
      connection,
    )
  }

  private headerContainsToken(name: string, token: string): bool {
    lowerName := name.toLowerCase()
    lowerToken := token.toLowerCase()
    for header of this.headers {
      if header.name.toLowerCase() != lowerName {
        continue
      }
      value := header.value
      let remaining = value
      while true {
        separator := remaining.indexOf(",")
        let part = remaining
        if separator >= 0 {
          part = remaining.substring(0, separator)
          remaining = remaining.slice(separator + 1)
        } else {
          remaining = ""
        }
        if part.trim().toLowerCase() == lowerToken {
          return true
        }
        if remaining == "" {
          break
        }
      }
    }
    return false
  }
}

export function requestFromExchange(exchange: NativeExchange): Request {
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
    keepAlive: exchange.keepAlive(),
    responder: exchange.responder(),
  }
}
