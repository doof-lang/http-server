#pragma once

#include "native_http_server_common.hpp"

namespace doof_http_server {
namespace detail {

inline std::string statusText(int32_t status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 418: return "I'm a teapot";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "HTTP " + std::to_string(status);
    }
}

inline std::string trim(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

inline std::string toLower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

inline bool isTokenChar(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return true;
    }
    if (ch >= 'A' && ch <= 'Z') {
        return true;
    }
    if (ch >= 'a' && ch <= 'z') {
        return true;
    }
    switch (ch) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

inline bool isHeaderName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (unsigned char ch : name) {
        if (!isTokenChar(ch)) {
            return false;
        }
    }
    return true;
}

inline bool isMethod(std::string_view method) {
    return isHeaderName(method);
}

inline bool isVisibleRequestTarget(std::string_view target) {
    if (target.empty()) {
        return false;
    }
    for (unsigned char ch : target) {
        if (ch <= 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

inline bool isHeaderValue(std::string_view value) {
    for (unsigned char ch : value) {
        if (ch == '\t' || (ch >= 0x20 && ch != 0x7f)) {
            continue;
        }
        return false;
    }
    return true;
}

inline bool parseContentLength(const std::string& text, int64_t& out) {
    if (text.empty()) {
        return false;
    }
    int64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        if (value > (INT64_MAX - (ch - '0')) / 10) {
            return false;
        }
        value = value * 10 + static_cast<int64_t>(ch - '0');
    }
    out = value;
    return true;
}

inline int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

inline bool parseChunkSize(std::string_view text, int64_t& out) {
    const size_t extension = text.find(';');
    const std::string_view digits = extension == std::string_view::npos ? text : text.substr(0, extension);
    if (digits.empty()) {
        return false;
    }
    int64_t value = 0;
    for (char ch : digits) {
        const int digit = hexValue(ch);
        if (digit < 0) {
            return false;
        }
        if (value > (INT64_MAX - digit) / 16) {
            return false;
        }
        value = value * 16 + digit;
    }
    out = value;
    return true;
}

inline bool parseTransferEncoding(
    const std::string& value,
    bool& chunked,
    std::string& error
) {
    size_t start = 0;
    while (start <= value.size()) {
        const size_t separator = value.find(',', start);
        const size_t end = separator == std::string::npos ? value.size() : separator;
        const std::string token = toLower(trim(value.substr(start, end - start)));
        if (token.empty()) {
            error = "malformed-request|invalid Transfer-Encoding header";
            return false;
        }
        if (token == "chunked") {
            if (chunked || separator != std::string::npos) {
                error = "unsupported-transfer-encoding|unsupported Transfer-Encoding";
                return false;
            }
            chunked = true;
        } else if (token != "identity") {
            error = "unsupported-transfer-encoding|unsupported Transfer-Encoding";
            return false;
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return true;
}

struct ParsedRequest {
    std::string method;
    std::string target;
    std::string version;
    std::string headersText;
    std::shared_ptr<std::vector<uint8_t>> body;
    bool keepAlive = false;
};

inline bool headerValueHasToken(std::string value, std::string_view wanted) {
    value = toLower(std::move(value));
    size_t start = 0;
    while (start <= value.size()) {
        const size_t separator = value.find(',', start);
        const size_t end = separator == std::string::npos ? value.size() : separator;
        if (trim(value.substr(start, end - start)) == wanted) {
            return true;
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return false;
}

inline bool requestShouldKeepAlive(
    const std::string& version,
    bool connectionClose,
    bool connectionKeepAlive
) {
    if (connectionClose) {
        return false;
    }
    if (version == "HTTP/1.1") {
        return true;
    }
    if (version == "HTTP/1.0") {
        return connectionKeepAlive;
    }
    return false;
}

inline bool responseRequestsClose(const std::string& headersText) {
    std::istringstream lines(headersText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        if (toLower(line.substr(0, separator)) == "connection" &&
            headerValueHasToken(trim(line.substr(separator + 1)), "close")) {
            return true;
        }
    }
    return false;
}

inline std::string normalizedResponseHeaders(
    const std::string& headersText,
    size_t bodySize,
    bool keepAlive
) {
    std::string normalized;
    std::istringstream lines(headersText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        const std::string lowerName = toLower(line.substr(0, separator));
        if (lowerName == "content-length" || lowerName == "connection") {
            continue;
        }
        normalized += line + "\r\n";
    }
    normalized += "Content-Length: " + std::to_string(bodySize) + "\r\n";
    normalized += std::string("Connection: ") + (keepAlive ? "keep-alive" : "close") + "\r\n";
    return normalized;
}

inline std::string normalizedChunkedResponseHeaders(
    const std::string& headersText,
    bool keepAlive
) {
    std::string normalized;
    std::istringstream lines(headersText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        const std::string lowerName = toLower(line.substr(0, separator));
        if (lowerName == "content-length" || lowerName == "connection" || lowerName == "transfer-encoding") {
            continue;
        }
        normalized += line + "\r\n";
    }
    normalized += "Transfer-Encoding: chunked\r\n";
    normalized += std::string("Connection: ") + (keepAlive ? "keep-alive" : "close") + "\r\n";
    return normalized;
}

enum class ParseStatus {
    NeedMore,
    Complete,
    Error,
};

struct ParseAttempt {
    ParseStatus status = ParseStatus::NeedMore;
    ParsedRequest request;
    std::string error;
};

inline ParseAttempt parseChunkedBody(
    std::string& buffered,
    size_t bodyStart,
    int64_t maxBodyBytes,
    ParsedRequest& parsed
) {
    size_t cursor = bodyStart;
    int64_t decodedSize = 0;
    auto body = std::make_shared<std::vector<uint8_t>>();

    while (true) {
        const size_t lineEnd = buffered.find("\r\n", cursor);
        if (lineEnd == std::string::npos) {
            return ParseAttempt {};
        }

        int64_t chunkSize = 0;
        if (!parseChunkSize(std::string_view(buffered).substr(cursor, lineEnd - cursor), chunkSize)) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid chunk size" };
        }
        if (chunkSize > maxBodyBytes - decodedSize) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "body-too-large|request body exceeds configured maxBodyBytes" };
        }
        cursor = lineEnd + 2;

        if (chunkSize == 0) {
            if (buffered.size() < cursor + 2) {
                return ParseAttempt {};
            }
            if (buffered.compare(cursor, 2, "\r\n") == 0) {
                cursor += 2;
                parsed.body = std::move(body);
                buffered.erase(0, cursor);
                return ParseAttempt { ParseStatus::Complete, std::move(parsed), "" };
            }

            const size_t trailerEnd = buffered.find("\r\n\r\n", cursor);
            if (trailerEnd == std::string::npos) {
                return ParseAttempt {};
            }
            std::istringstream trailers(buffered.substr(cursor, trailerEnd - cursor));
            std::string trailerLine;
            while (std::getline(trailers, trailerLine)) {
                if (!trailerLine.empty() && trailerLine.back() == '\r') {
                    trailerLine.pop_back();
                }
                const size_t separator = trailerLine.find(':');
                if (separator == std::string::npos || separator == 0 ||
                    !isHeaderName(trailerLine.substr(0, separator)) ||
                    !isHeaderValue(trim(trailerLine.substr(separator + 1)))) {
                    return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid trailer line" };
                }
            }
            cursor = trailerEnd + 4;
            parsed.body = std::move(body);
            buffered.erase(0, cursor);
            return ParseAttempt { ParseStatus::Complete, std::move(parsed), "" };
        }

        const size_t chunkSizeBytes = static_cast<size_t>(chunkSize);
        if (chunkSizeBytes > SIZE_MAX - cursor - 2) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid chunk size" };
        }
        const size_t chunkEnd = cursor + chunkSizeBytes;
        if (buffered.size() < chunkEnd + 2) {
            return ParseAttempt {};
        }
        if (buffered.compare(chunkEnd, 2, "\r\n") != 0) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid chunk terminator" };
        }

        body->insert(body->end(),
                     buffered.begin() + static_cast<std::ptrdiff_t>(cursor),
                     buffered.begin() + static_cast<std::ptrdiff_t>(chunkEnd));
        decodedSize += chunkSize;
        cursor = chunkEnd + 2;
    }
}

inline ParseAttempt parseRequest(std::string& buffered, int64_t maxBodyBytes) {
    constexpr size_t maxHeaderBytes = 64 * 1024;
    constexpr size_t maxHeaderCount = 100;
    const size_t headerEnd = buffered.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        if (buffered.size() > maxHeaderBytes) {
            return ParseAttempt {
                ParseStatus::Error,
                ParsedRequest {},
                "headers-too-large|HTTP request headers exceed 65536 bytes",
            };
        }
        return ParseAttempt {};
    }
    if (headerEnd + 4 > maxHeaderBytes) {
        return ParseAttempt {
            ParseStatus::Error,
            ParsedRequest {},
            "headers-too-large|HTTP request headers exceed 65536 bytes",
        };
    }

    const std::string headerBlock = buffered.substr(0, headerEnd);
    std::istringstream lines(headerBlock);
    std::string requestLine;
    if (!std::getline(lines, requestLine)) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|missing request line" };
    }
    if (!requestLine.empty() && requestLine.back() == '\r') {
        requestLine.pop_back();
    }

    std::istringstream requestLineStream(requestLine);
    ParsedRequest parsed;
    if (!(requestLineStream >> parsed.method >> parsed.target >> parsed.version)) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid request line" };
    }
    std::string unexpected;
    if (requestLineStream >> unexpected) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid request line" };
    }
    if (!isMethod(parsed.method)) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid request method" };
    }
    if (!isVisibleRequestTarget(parsed.target)) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid request target" };
    }
    if (parsed.version != "HTTP/1.0" && parsed.version != "HTTP/1.1") {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid HTTP version" };
    }

    int64_t contentLength = 0;
    bool sawContentLength = false;
    bool sawHost = false;
    bool sawChunkedTransferEncoding = false;
    bool connectionClose = false;
    bool connectionKeepAlive = false;
    size_t headerCount = 0;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const size_t separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid header line" };
        }

        const std::string name = line.substr(0, separator);
        const std::string value = trim(line.substr(separator + 1));
        ++headerCount;
        if (headerCount > maxHeaderCount) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "headers-too-large|HTTP request has too many headers" };
        }
        if (!isHeaderName(name) || !isHeaderValue(value)) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid header line" };
        }
        const std::string lowerName = toLower(name);

        if (lowerName == "content-length") {
            if (sawContentLength) {
                return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|duplicate Content-Length header" };
            }
            if (!parseContentLength(value, contentLength)) {
                return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid Content-Length header" };
            }
            sawContentLength = true;
        }
        if (lowerName == "transfer-encoding") {
            std::string transferEncodingError;
            if (!parseTransferEncoding(value, sawChunkedTransferEncoding, transferEncodingError)) {
                return ParseAttempt { ParseStatus::Error, ParsedRequest {}, transferEncodingError };
            }
        }
        if (lowerName == "host") {
            if (sawHost || value.empty()) {
                return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid Host header" };
            }
            sawHost = true;
        }
        if (lowerName == "connection") {
            connectionClose = connectionClose || headerValueHasToken(value, "close");
            connectionKeepAlive = connectionKeepAlive || headerValueHasToken(value, "keep-alive");
        }

        parsed.headersText += name + ": " + value + "\r\n";
    }

    if (parsed.version == "HTTP/1.1" && !sawHost) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|missing Host header" };
    }

    if (contentLength < 0 || contentLength > maxBodyBytes) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "body-too-large|request body exceeds configured maxBodyBytes" };
    }

    const size_t bodyStart = headerEnd + 4;
    if (sawChunkedTransferEncoding) {
        if (sawContentLength) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|Content-Length is not allowed with chunked Transfer-Encoding" };
        }
        parsed.keepAlive = requestShouldKeepAlive(parsed.version, connectionClose, connectionKeepAlive);
        return parseChunkedBody(buffered, bodyStart, maxBodyBytes, parsed);
    }

    const size_t totalSize = bodyStart + static_cast<size_t>(contentLength);
    if (buffered.size() < totalSize) {
        return ParseAttempt {};
    }

    auto body = std::make_shared<std::vector<uint8_t>>();
    body->reserve(static_cast<size_t>(contentLength));
    if (contentLength > 0) {
        body->insert(body->end(), buffered.begin() + static_cast<std::ptrdiff_t>(bodyStart),
                     buffered.begin() + static_cast<std::ptrdiff_t>(totalSize));
    }

    parsed.body = std::move(body);
    parsed.keepAlive = requestShouldKeepAlive(parsed.version, connectionClose, connectionKeepAlive);
    buffered.erase(0, totalSize);
    return ParseAttempt { ParseStatus::Complete, std::move(parsed), "" };
}

inline std::vector<uint8_t> responseBytes(
    int32_t status,
    const std::string& headersText,
    const std::shared_ptr<std::vector<uint8_t>>& body,
    bool keepAlive
) {
    const auto safeBody = body ? body : std::make_shared<std::vector<uint8_t>>();
    const std::string head =
        "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n" +
        normalizedResponseHeaders(headersText, safeBody->size(), keepAlive) +
        "\r\n";
    std::vector<uint8_t> bytes;
    bytes.reserve(head.size() + safeBody->size());
    bytes.insert(bytes.end(), head.begin(), head.end());
    bytes.insert(bytes.end(), safeBody->begin(), safeBody->end());
    return bytes;
}

inline std::vector<uint8_t> chunkedResponseHeadBytes(
    int32_t status,
    const std::string& headersText,
    bool keepAlive
) {
    const std::string head =
        "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n" +
        normalizedChunkedResponseHeaders(headersText, keepAlive) +
        "\r\n";
    return std::vector<uint8_t>(head.begin(), head.end());
}

inline std::vector<uint8_t> chunkedResponseChunkBytes(
    const std::shared_ptr<std::vector<uint8_t>>& chunk
) {
    if (!chunk || chunk->empty()) {
        return {};
    }

    std::ostringstream size;
    size << std::hex << chunk->size();
    std::string prefix = size.str() + "\r\n";
    std::vector<uint8_t> bytes;
    bytes.reserve(prefix.size() + chunk->size() + 2);
    bytes.insert(bytes.end(), prefix.begin(), prefix.end());
    bytes.insert(bytes.end(), chunk->begin(), chunk->end());
    bytes.push_back(static_cast<uint8_t>('\r'));
    bytes.push_back(static_cast<uint8_t>('\n'));
    return bytes;
}

inline std::vector<uint8_t> chunkedResponseEndBytes() {
    const std::string end = "0\r\n\r\n";
    return std::vector<uint8_t>(end.begin(), end.end());
}

inline std::vector<uint8_t> simpleResponseBytes(int32_t status, const std::string& body) {
    return responseBytes(
        status,
        "Content-Type: text/plain; charset=utf-8\r\n",
        std::make_shared<std::vector<uint8_t>>(body.begin(), body.end()),
        false
    );
}

}  // namespace detail
}  // namespace doof_http_server
