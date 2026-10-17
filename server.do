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

export class Server {
  readonly host: string
  readonly port: int
  private readonly native: NativeHttpServer

  static listen(
    options: ServerOptions,
    onRequest: (request: Request): none,
  ): Result<Server, ServerError> {
    started := NativeHttpServer.listen(
      options.host,
      options.port,
      options.maxBodyBytes,
      options.idleTimeoutMillis,
      options.responseTimeoutMillis,
      options.maxRequestsPerConnection,
      options.keepsAlive,
      (exchange: NativeExchange): none => onRequest(requestFromExchange(exchange)),
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

  close(): Result<none, ServerError> {
    return mapNativeVoid(this.native.close())
  }
}
