export function validateWebSocketHandshake(
  method: string,
  version: string,
  headersText: string,
): Result<string, string> {
  if method != "GET" {
    return Failure { error: "bad-websocket-handshake|websocket upgrade requires GET" }
  }
  if version != "HTTP/1.1" {
    return Failure { error: "bad-websocket-handshake|websocket upgrade requires HTTP/1.1" }
  }

  upgrade := headerValue(headersText, "upgrade") else {
    return Failure { error: "bad-websocket-handshake|missing Upgrade: websocket" }
  }
  if upgrade.toLowerCase() != "websocket" {
    return Failure { error: "bad-websocket-handshake|missing Upgrade: websocket" }
  }

  connection := headerValue(headersText, "connection") else {
    return Failure { error: "bad-websocket-handshake|missing Connection: upgrade" }
  }
  if !headerValueHasToken(connection, "upgrade") {
    return Failure { error: "bad-websocket-handshake|missing Connection: upgrade" }
  }

  socketVersion := headerValue(headersText, "sec-websocket-version") else {
    return Failure { error: "bad-websocket-handshake|unsupported websocket version" }
  }
  if socketVersion != "13" {
    return Failure { error: "bad-websocket-handshake|unsupported websocket version" }
  }

  key := headerValue(headersText, "sec-websocket-key") else {
    return Failure { error: "bad-websocket-handshake|invalid Sec-WebSocket-Key" }
  }
  if !isWebSocketKey(key) {
    return Failure { error: "bad-websocket-handshake|invalid Sec-WebSocket-Key" }
  }

  return Success { value: key }
}

function headerValue(headersText: string, wantedName: string): string | null {
  lowerWanted := wantedName.toLowerCase()
  let remaining = headersText
  while remaining.length > 0 {
    newline := remaining.indexOf("\n")
    let rawLine = remaining
    if newline >= 0 {
      rawLine = remaining.substring(0, newline)
      remaining = remaining.slice(newline + 1)
    } else {
      remaining = ""
    }
    line := rawLine.trim()
    separator := line.indexOf(":")
    if separator <= 0 {
      continue
    }
    name := line.substring(0, separator).toLowerCase()
    if name == lowerWanted {
      return line.slice(separator + 1).trim()
    }
  }
  return null
}

function headerValueHasToken(value: string, wanted: string): bool {
  lowerWanted := wanted.toLowerCase()
  let remaining = value.toLowerCase()
  while true {
    separator := remaining.indexOf(",")
    let token = remaining
    if separator >= 0 {
      token = remaining.substring(0, separator)
      remaining = remaining.slice(separator + 1)
    } else {
      remaining = ""
    }
    if token.trim() == lowerWanted {
      return true
    }
    if remaining == "" {
      return false
    }
  }
  return false
}

function isWebSocketKey(value: string): bool {
  if value.length != 24 {
    return false
  }
  if value.slice(22) != "==" {
    return false
  }
  let index = 0
  while index < value.length {
    ch := value.substring(index, index + 1)
    if isBase64Char(ch) {
      index += 1
      continue
    }
    return false
  }
  return true
}

function isBase64Char(ch: string): bool {
  return ("A" <= ch && ch <= "Z") ||
    ("a" <= ch && ch <= "z") ||
    ("0" <= ch && ch <= "9") ||
    ch == "+" ||
    ch == "/" ||
    ch == "="
}
