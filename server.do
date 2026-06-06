import { ChannelSender, SendError } from "std/event"

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
}

export class Server {
  readonly host: string
  readonly port: int
  private readonly native: NativeHttpServer

  static listen(
    options: ServerOptions,
    requests: ChannelSender<Request>,
  ): Result<Server, ServerError> {
    started := NativeHttpServer.listen(
      options.host,
      options.port,
      options.maxBodyBytes,
      options.idleTimeoutMillis,
      options.responseTimeoutMillis,
      options.maxRequestsPerConnection,
      (exchange: NativeExchange): int => requestDisposition(requests, exchange),
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

  close(): Result<void, ServerError> {
    return mapNativeVoid(this.native.close())
  }
}

function requestDisposition(
  requests: ChannelSender<Request>,
  exchange: NativeExchange,
): int {
  delivered := requests.send(requestFromExchange(exchange))
  return case delivered {
    _: Success -> 0,
    f: Failure -> case f.error {
      SendError.Full -> 1,
      _ -> 2,
    }
  }
}
