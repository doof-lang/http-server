export class ServerError {
  readonly kind: string
  readonly message: string
}

export function parseServerError(raw: string): ServerError {
  separator := raw.indexOf("|")
  if separator < 0 {
    return ServerError {
      kind: "server",
      message: raw,
    }
  }

  return ServerError {
    kind: raw.substring(0, separator),
    message: raw.slice(separator + 1),
  }
}

export function mapNativeVoid(result: Result<none, string>): Result<none, ServerError> {
  return case result {
    _: Success -> Success {},
    f: Failure -> Failure {
      error: parseServerError(f.error)
    }
  }
}
