import { ServerError, mapNativeVoid, parseServerError } from "./errors"
import { NativeExchange, NativeHttpServer } from "./native"
import { Request, requestFromExchange } from "./request"

export class ServerOptions {
  readonly host: string = "127.0.0.1"
  readonly port: int = 0
  readonly maxBodyBytes: long = 1_048_576L
  readonly idleTimeoutMillis: int = 30_000
  readonly responseTimeoutMillis: int = 30_000
  readonly maxRequestsPerConnection: int = 0
  // While open, the server keeps `runMainEventLoop()` from returning.
  readonly keepsAlive: bool = true
}

// A request callback that runs on the actor that created it. Create one inside
// each worker actor with `requestHandler` and pass them all to
// `Server.listenPool`; the server then posts requests straight to the workers.
export class RequestHandler {
  private readonly onExchange: (exchange: NativeExchange): none
}

// Wraps `onRequest` so the server can post to it directly. Call it on the
// actor that should handle the requests: the returned handler belongs to the
// calling actor, and `onRequest` must belong to that actor too.
export function requestHandler(onRequest: (request: Request): none): RequestHandler {
  return RequestHandler {
    onExchange: (exchange: NativeExchange): none => onRequest(requestFromExchange(exchange)),
  }
}

export class Server {
  readonly host: string
  readonly port: int
  private readonly native: NativeHttpServer

  // Posts every request to `onRequest` on the actor that created it.
  static listen(
    options: ServerOptions,
    onRequest: (request: Request): none,
  ): Result<Server, ServerError> {
    return Server.start(options, requestHandler(onRequest))
  }

  // Spreads requests across `handlers` in round-robin order, skipping any
  // whose mailbox is full, and answers 503 when all of them are full. Each
  // handler runs requests one at a time on its own actor, so handlers created
  // on different actors run requests in parallel.
  static listenPool(
    options: ServerOptions,
    handlers: readonly RequestHandler[],
  ): Result<Server, ServerError> {
    if handlers.length == 0 {
      return Failure {
        error: ServerError { kind: "config", message: "handlers must not be empty" }
      }
    }
    server := Server.start(options, handlers[0]) else error {
      return Failure { error }
    }
    if handlers.length > 1 {
      server.setHandlers(handlers) else error {
        server.close() else {}
        return Failure { error }
      }
    }
    return Success { value: server }
  }

  // Replaces the handlers that receive new requests. Requests already posted
  // to a handler stay with it.
  setHandlers(handlers: readonly RequestHandler[]): Result<none, ServerError> {
    callbacks: ((exchange: NativeExchange): none)[] := []
    for handler of handlers {
      callbacks.push(handler.onExchange)
    }
    return mapNativeVoid(this.native.setHandlers(callbacks))
  }

  // Requests answered with 503 because every handler's mailbox was full.
  shedCount(): long => this.native.shedCount()

  close(): Result<none, ServerError> {
    return mapNativeVoid(this.native.close())
  }

  private static start(options: ServerOptions, handler: RequestHandler): Result<Server, ServerError> {
    started := NativeHttpServer.listen(
      options.host,
      options.port,
      options.maxBodyBytes,
      options.idleTimeoutMillis,
      options.responseTimeoutMillis,
      options.maxRequestsPerConnection,
      options.keepsAlive,
      handler.onExchange,
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
}
