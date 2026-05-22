#pragma once

#include "native_http_server_protocol.hpp"

namespace doof_http_server {
namespace detail {

enum class WebSocketFrameParseStatus {
    NeedMore,
    Complete,
    Error,
};

struct ParsedWebSocketFrame {
    WebSocketFrameParseStatus status = WebSocketFrameParseStatus::NeedMore;
    bool fin = false;
    uint8_t opcode = 0;
    std::vector<uint8_t> payload;
    std::string error;
    int32_t closeCode = 1002;
};

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

inline ParsedWebSocketFrame parseClientWebSocketFrame(std::string& readBuffer, int64_t maxPayloadBytes) {
    if (readBuffer.size() < 2) {
        return ParsedWebSocketFrame {};
    }

    const auto b0 = static_cast<uint8_t>(readBuffer[0]);
    const auto b1 = static_cast<uint8_t>(readBuffer[1]);
    ParsedWebSocketFrame frame;
    frame.fin = (b0 & 0x80) != 0;
    frame.opcode = b0 & 0x0f;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t length = b1 & 0x7f;
    size_t offset = 2;

    if (!masked) {
        frame.status = WebSocketFrameParseStatus::Error;
        frame.error = "protocol-error|websocket client frames must be masked";
        return frame;
    }
    if ((b0 & 0x70) != 0) {
        frame.status = WebSocketFrameParseStatus::Error;
        frame.error = "protocol-error|websocket reserved bits are not supported";
        return frame;
    }
    if (length == 126) {
        if (readBuffer.size() < offset + 2) {
            return frame;
        }
        length = (static_cast<uint64_t>(static_cast<uint8_t>(readBuffer[offset])) << 8) |
                 static_cast<uint64_t>(static_cast<uint8_t>(readBuffer[offset + 1]));
        offset += 2;
    } else if (length == 127) {
        if (readBuffer.size() < offset + 8) {
            return frame;
        }
        length = 0;
        for (int i = 0; i < 8; ++i) {
            length = (length << 8) | static_cast<uint64_t>(static_cast<uint8_t>(readBuffer[offset + static_cast<size_t>(i)]));
        }
        offset += 8;
    }
    if (length > static_cast<uint64_t>(maxPayloadBytes)) {
        frame.status = WebSocketFrameParseStatus::Error;
        frame.error = "message-too-large|websocket message exceeds configured maxBodyBytes";
        frame.closeCode = 1009;
        return frame;
    }
    if (frame.opcode >= 0x8 && (!frame.fin || length > 125)) {
        frame.status = WebSocketFrameParseStatus::Error;
        frame.error = "protocol-error|invalid websocket control frame";
        return frame;
    }
    if (readBuffer.size() < offset + 4 + static_cast<size_t>(length)) {
        return frame;
    }

    uint8_t mask[4] = {
        static_cast<uint8_t>(readBuffer[offset]),
        static_cast<uint8_t>(readBuffer[offset + 1]),
        static_cast<uint8_t>(readBuffer[offset + 2]),
        static_cast<uint8_t>(readBuffer[offset + 3]),
    };
    offset += 4;
    frame.payload.resize(static_cast<size_t>(length));
    for (size_t i = 0; i < frame.payload.size(); ++i) {
        frame.payload[i] = static_cast<uint8_t>(readBuffer[offset + i]) ^ mask[i % 4];
    }
    readBuffer.erase(0, offset + frame.payload.size());
    frame.status = WebSocketFrameParseStatus::Complete;
    return frame;
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
