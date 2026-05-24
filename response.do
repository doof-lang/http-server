import { BlobBuilder } from "std/blob"
import { HttpHeader } from "std/http"
import { formatJsonValue } from "std/json"

import { hasHeader } from "./headers"

export enum ResponseCompression {
  Default,
  None,
  Compress,
}

export class Response {
  readonly status: int
  readonly headers: readonly HttpHeader[]
  readonly body: readonly byte[] | Stream<readonly byte[]>
  readonly compression: ResponseCompression = ResponseCompression.Default

  static empty(status: int = 204): Response {
    return Response {
      status,
      headers: readonly [],
      body: emptyBytes(),
      compression: ResponseCompression.None,
    }
  }

  static blob(
    status: int,
    body: readonly byte[],
    headers: readonly HttpHeader[] = [],
    compression: ResponseCompression = ResponseCompression.Default,
  ): Response {
    return Response {
      status,
      headers,
      body,
      compression,
    }
  }

  static stream(
    status: int,
    body: Stream<readonly byte[]>,
    headers: readonly HttpHeader[] = [],
    compression: ResponseCompression = ResponseCompression.Default,
  ): Response {
    return Response {
      status,
      headers,
      body,
      compression,
    }
  }

  static text(
    status: int,
    body: string,
    headers: readonly HttpHeader[] = [],
    compression: ResponseCompression = ResponseCompression.Default,
  ): Response {
    return Response.blob(
      status,
      encodeText(body),
      withDefaultContentType(headers, "text/plain; charset=utf-8"),
      compression,
    )
  }

  static html(
    status: int,
    body: string,
    headers: readonly HttpHeader[] = [],
    compression: ResponseCompression = ResponseCompression.Default,
  ): Response {
    return Response.blob(
      status,
      encodeText(body),
      withDefaultContentType(headers, "text/html; charset=utf-8"),
      compression,
    )
  }

  static jsonValue(
    status: int,
    body: JsonValue,
    headers: readonly HttpHeader[] = [],
    compression: ResponseCompression = ResponseCompression.Default,
  ): Response {
    return Response.blob(
      status,
      encodeText(formatJsonValue(body)),
      withDefaultContentType(headers, "application/json; charset=utf-8"),
      compression,
    )
  }
}

function withDefaultContentType(
  headers: readonly HttpHeader[],
  contentType: string,
): readonly HttpHeader[] {
  if hasHeader(headers, "Content-Type") {
    return headers
  }

  merged: HttpHeader[] := []
  for header of headers {
    merged.push(header)
  }
  merged.push(HttpHeader {
    name: "Content-Type",
    value: contentType,
  })
  return merged.buildReadonly()
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
