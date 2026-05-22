#pragma once

#include "native_http_server_protocol.hpp"

#include <array>

namespace doof_http_server {
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

inline std::string websocketAccept(const std::string& key) {
    const auto digest = sha1Bytes(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64Encode(digest.data(), digest.size());
}

}  // namespace detail
}  // namespace doof_http_server
