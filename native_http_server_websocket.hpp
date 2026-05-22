#pragma once

#include "native_http_server_protocol.hpp"

#include <array>

namespace doof_http_server {

enum class NativeWebSocketState : int32_t {
    Connecting = 0,
    Open = 1,
    Closing = 2,
    Closed = 3,
    Error = 4,
};

enum class NativeWebSocketEventKind : int32_t {
    Open = 0,
    Text = 1,
    Binary = 2,
    Writable = 3,
    Close = 4,
    Error = 5,
};

class NativeWebSocketEvent {
public:
    NativeWebSocketEvent(
        NativeWebSocketEventKind kind,
        std::string text,
        std::shared_ptr<std::vector<uint8_t>> bytes,
        int32_t code,
        std::string reason,
        bool wasClean,
        std::string error
    )
        : kind_(kind),
          text_(std::move(text)),
          bytes_(std::move(bytes)),
          code_(code),
          reason_(std::move(reason)),
          wasClean_(wasClean),
          error_(std::move(error)) {}

    int32_t kind() const { return static_cast<int32_t>(kind_); }
    std::string text() const { return text_; }
    std::shared_ptr<std::vector<uint8_t>> bytes() const { return bytes_ ? bytes_ : std::make_shared<std::vector<uint8_t>>(); }
    int32_t code() const { return code_; }
    std::string reason() const { return reason_; }
    bool wasClean() const { return wasClean_; }
    std::string error() const { return error_; }

private:
    NativeWebSocketEventKind kind_;
    std::string text_;
    std::shared_ptr<std::vector<uint8_t>> bytes_;
    int32_t code_;
    std::string reason_;
    bool wasClean_;
    std::string error_;
};

class NativeWebSocketConnection : public std::enable_shared_from_this<NativeWebSocketConnection> {
public:
    using EventCallback = std::function<void(std::shared_ptr<NativeWebSocketEvent>)>;
    using Sender = std::function<doof::Result<void, std::string>(
        int32_t opcode,
        const std::shared_ptr<std::vector<uint8_t>>& payload,
        int32_t closeCode,
        const std::string& closeReason
    )>;

    static std::shared_ptr<NativeWebSocketConnection> create() {
        return std::make_shared<NativeWebSocketConnection>();
    }

    void setEventCallback(EventCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    void attach(Sender sender) {
        std::lock_guard<std::mutex> lock(mutex_);
        sender_ = std::move(sender);
    }

    int32_t state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int32_t>(state_);
    }

    doof::Result<void, std::string> sendText(const std::string& text) {
        auto payload = std::make_shared<std::vector<uint8_t>>(text.begin(), text.end());
        return send(0x1, payload, 0, "");
    }

    doof::Result<void, std::string> sendBinary(const std::shared_ptr<std::vector<uint8_t>>& bytes) {
        return send(0x2, bytes ? bytes : std::make_shared<std::vector<uint8_t>>(), 0, "");
    }

    doof::Result<void, std::string> ping() {
        return send(0x9, std::make_shared<std::vector<uint8_t>>(), 0, "");
    }

    doof::Result<void, std::string> close(int32_t code, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == NativeWebSocketState::Closed || state_ == NativeWebSocketState::Error) {
                return doof::Result<void, std::string>::failure("closed|websocket is closed");
            }
            state_ = NativeWebSocketState::Closing;
        }
        return send(0x8, std::make_shared<std::vector<uint8_t>>(), code, reason);
    }

    void markOpen() {
        setState(NativeWebSocketState::Open);
        emit(NativeWebSocketEventKind::Open, "", {}, 0, "", true, "");
    }

    void markError(const std::string& error) {
        setState(NativeWebSocketState::Error);
        emit(NativeWebSocketEventKind::Error, "", {}, 0, "", false, error);
    }

    void markClosed(int32_t code, const std::string& reason, bool wasClean) {
        setState(NativeWebSocketState::Closed);
        emit(NativeWebSocketEventKind::Close, "", {}, code, reason, wasClean, "");
    }

    void emitText(const std::string& text) {
        emit(NativeWebSocketEventKind::Text, text, {}, 0, "", true, "");
    }

    void emitBinary(std::shared_ptr<std::vector<uint8_t>> bytes) {
        emit(NativeWebSocketEventKind::Binary, "", std::move(bytes), 0, "", true, "");
    }

    void emitWritable() {
        emit(NativeWebSocketEventKind::Writable, "", {}, 0, "", true, "");
    }

private:
    doof::Result<void, std::string> send(
        int32_t opcode,
        const std::shared_ptr<std::vector<uint8_t>>& payload,
        int32_t closeCode,
        const std::string& closeReason
    ) {
        Sender sender;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!sender_) {
                return doof::Result<void, std::string>::failure("not-open|websocket is not open");
            }
            if (state_ != NativeWebSocketState::Open && !(opcode == 0x8 && state_ == NativeWebSocketState::Closing)) {
                return doof::Result<void, std::string>::failure("not-open|websocket is not open");
            }
            sender = sender_;
        }
        return sender(opcode, payload, closeCode, closeReason);
    }

    void setState(NativeWebSocketState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }

    void emit(
        NativeWebSocketEventKind kind,
        std::string text,
        std::shared_ptr<std::vector<uint8_t>> bytes,
        int32_t code,
        std::string reason,
        bool wasClean,
        std::string error
    ) {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (callback) {
            callback(std::make_shared<NativeWebSocketEvent>(
                kind,
                std::move(text),
                std::move(bytes),
                code,
                std::move(reason),
                wasClean,
                std::move(error)
            ));
        }
    }

    mutable std::mutex mutex_;
    NativeWebSocketState state_ = NativeWebSocketState::Connecting;
    EventCallback callback_;
    Sender sender_;
};

namespace detail {

inline uint32_t rotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

inline std::array<uint8_t, 20> sha1Bytes(const std::string& input) {
    uint64_t bitLength = static_cast<uint64_t>(input.size()) * 8;
    std::vector<uint8_t> data(input.begin(), input.end());
    data.push_back(0x80);
    while ((data.size() % 64) != 56) {
        data.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<uint8_t>((bitLength >> (i * 8)) & 0xff));
    }

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xefcdab89;
    uint32_t h2 = 0x98badcfe;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xc3d2e1f0;

    for (size_t offset = 0; offset < data.size(); offset += 64) {
        uint32_t w[80] {};
        for (int i = 0; i < 16; ++i) {
            const size_t j = offset + static_cast<size_t>(i * 4);
            w[i] = (static_cast<uint32_t>(data[j]) << 24) |
                   (static_cast<uint32_t>(data[j + 1]) << 16) |
                   (static_cast<uint32_t>(data[j + 2]) << 8) |
                   static_cast<uint32_t>(data[j + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t temp = rotateLeft(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> out {};
    const uint32_t words[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; ++i) {
        out[static_cast<size_t>(i * 4)] = static_cast<uint8_t>((words[i] >> 24) & 0xff);
        out[static_cast<size_t>(i * 4 + 1)] = static_cast<uint8_t>((words[i] >> 16) & 0xff);
        out[static_cast<size_t>(i * 4 + 2)] = static_cast<uint8_t>((words[i] >> 8) & 0xff);
        out[static_cast<size_t>(i * 4 + 3)] = static_cast<uint8_t>(words[i] & 0xff);
    }
    return out;
}

inline std::string base64Encode(const uint8_t* data, size_t size) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
        const uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(alphabet[(triple >> 18) & 0x3f]);
        out.push_back(alphabet[(triple >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? alphabet[(triple >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < size ? alphabet[triple & 0x3f] : '=');
    }
    return out;
}

inline bool isBase64Key(std::string_view value) {
    if (value.size() != 24) {
        return false;
    }
    for (char ch : value) {
        const bool ok = (ch >= 'A' && ch <= 'Z') ||
                        (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '+' || ch == '/' || ch == '=';
        if (!ok) {
            return false;
        }
    }
    return value.substr(22) == "==";
}

inline std::string websocketAccept(const std::string& key) {
    const auto digest = sha1Bytes(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64Encode(digest.data(), digest.size());
}

inline bool findHeaderValue(const std::string& headersText, const std::string& wantedName, std::string& out) {
    std::istringstream lines(headersText);
    std::string line;
    const std::string lowerWanted = toLower(wantedName);
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        if (toLower(line.substr(0, separator)) == lowerWanted) {
            out = trim(line.substr(separator + 1));
            return true;
        }
    }
    return false;
}

inline doof::Result<std::string, std::string> validateWebSocketHandshake(const ParsedRequest& request) {
    if (request.method != "GET") {
        return doof::Result<std::string, std::string>::failure("bad-websocket-handshake|websocket upgrade requires GET");
    }
    if (request.version != "HTTP/1.1") {
        return doof::Result<std::string, std::string>::failure("bad-websocket-handshake|websocket upgrade requires HTTP/1.1");
    }

    std::string upgrade;
    if (!findHeaderValue(request.headersText, "upgrade", upgrade) || toLower(upgrade) != "websocket") {
        return doof::Result<std::string, std::string>::failure("bad-websocket-handshake|missing Upgrade: websocket");
    }

    std::string connection;
    if (!findHeaderValue(request.headersText, "connection", connection) || !headerValueHasToken(connection, "upgrade")) {
        return doof::Result<std::string, std::string>::failure("bad-websocket-handshake|missing Connection: upgrade");
    }

    std::string version;
    if (!findHeaderValue(request.headersText, "sec-websocket-version", version) || version != "13") {
        return doof::Result<std::string, std::string>::failure("bad-websocket-handshake|unsupported websocket version");
    }

    std::string key;
    if (!findHeaderValue(request.headersText, "sec-websocket-key", key) || !isBase64Key(key)) {
        return doof::Result<std::string, std::string>::failure("bad-websocket-handshake|invalid Sec-WebSocket-Key");
    }

    return doof::Result<std::string, std::string>::success(websocketAccept(key));
}

inline std::vector<uint8_t> websocketHandshakeBytes(
    const std::string& accept,
    const std::string& extraHeaders,
    const std::string& subprotocol
) {
    std::string head =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n";
    if (!subprotocol.empty()) {
        head += "Sec-WebSocket-Protocol: " + subprotocol + "\r\n";
    }
    head += extraHeaders;
    head += "\r\n";
    return std::vector<uint8_t>(head.begin(), head.end());
}

inline std::vector<uint8_t> websocketFrame(uint8_t opcode, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0f)));
    if (payload.size() <= 125) {
        out.push_back(static_cast<uint8_t>(payload.size()));
    } else if (payload.size() <= 65535) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(payload.size() & 0xff));
    } else {
        out.push_back(127);
        const uint64_t size = static_cast<uint64_t>(payload.size());
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<uint8_t>((size >> (i * 8)) & 0xff));
        }
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

inline std::vector<uint8_t> closePayload(int32_t code, const std::string& reason) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xff));
    payload.push_back(static_cast<uint8_t>(code & 0xff));
    payload.insert(payload.end(), reason.begin(), reason.end());
    return payload;
}

inline bool isValidUtf8(const std::vector<uint8_t>& bytes) {
    size_t i = 0;
    while (i < bytes.size()) {
        const uint8_t c = bytes[i];
        if (c <= 0x7f) {
            ++i;
            continue;
        }
        size_t needed = 0;
        uint32_t code = 0;
        if ((c & 0xe0) == 0xc0) {
            needed = 1;
            code = c & 0x1f;
            if (code == 0) {
                return false;
            }
        } else if ((c & 0xf0) == 0xe0) {
            needed = 2;
            code = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            needed = 3;
            code = c & 0x07;
        } else {
            return false;
        }
        if (i + needed >= bytes.size()) {
            return false;
        }
        for (size_t j = 1; j <= needed; ++j) {
            const uint8_t cc = bytes[i + j];
            if ((cc & 0xc0) != 0x80) {
                return false;
            }
            code = (code << 6) | (cc & 0x3f);
        }
        if ((needed == 1 && code < 0x80) ||
            (needed == 2 && code < 0x800) ||
            (needed == 3 && code < 0x10000) ||
            code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) {
            return false;
        }
        i += needed + 1;
    }
    return true;
}

}  // namespace detail
}  // namespace doof_http_server
