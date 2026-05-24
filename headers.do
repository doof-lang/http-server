import { HttpHeader } from "std/http"

export function parseHeaders(headerText: string): readonly HttpHeader[] {
  headers: HttpHeader[] := []
  lines := headerText.split("\r\n")
  for line of lines {
    if line == "" {
      continue
    }

    separator := line.indexOf(":")
    if separator <= 0 {
      continue
    }

    headers.push(HttpHeader {
      name: line.substring(0, separator).trim(),
      value: line.slice(separator + 1).trim(),
    })
  }
  return headers.buildReadonly()
}

export function renderHeaders(headers: readonly HttpHeader[]): string {
  let text = ""
  for header of headers {
    text += "${header.name}: ${header.value}\r\n"
  }
  return text
}

export function headersAreSafe(headers: readonly HttpHeader[]): bool {
  for header of headers {
    if header.name.contains("\r") || header.name.contains("\n") {
      return false
    }
    if header.value.contains("\r") || header.value.contains("\n") {
      return false
    }
  }
  return true
}

export function hasHeader(headers: readonly HttpHeader[], name: string): bool {
  lowerName := name.toLowerCase()
  for header of headers {
    if header.name.toLowerCase() == lowerName {
      return true
    }
  }
  return false
}

export function headerValue(headers: readonly HttpHeader[], name: string): string | null {
  lowerName := name.toLowerCase()
  for header of headers {
    if header.name.toLowerCase() == lowerName {
      return header.value
    }
  }
  return null
}

export function headerContainsTokenValue(value: string, token: string): bool {
  lowerToken := token.toLowerCase()
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
  return false
}

export function withHeader(headers: readonly HttpHeader[], name: string, value: string): readonly HttpHeader[] {
  merged: HttpHeader[] := []
  for header of headers {
    merged.push(header)
  }
  merged.push(HttpHeader {
    name,
    value,
  })
  return merged.buildReadonly()
}

export function withVaryAcceptEncoding(headers: readonly HttpHeader[]): readonly HttpHeader[] {
  vary := headerValue(headers, "Vary") else {
    return withHeader(headers, "Vary", "Accept-Encoding")
  }
  if headerContainsTokenValue(vary, "Accept-Encoding") {
    return headers
  }

  merged: HttpHeader[] := []
  for header of headers {
    if header.name.toLowerCase() == "vary" {
      merged.push(HttpHeader {
        name: header.name,
        value: header.value + ", Accept-Encoding",
      })
    } else {
      merged.push(header)
    }
  }
  return merged.buildReadonly()
}
