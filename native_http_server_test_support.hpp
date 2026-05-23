#pragma once

#include "doof_runtime.hpp"
#include "native_http_server_protocol.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

namespace doof_http_server_test {

class NativeHttpRequestParserFuzz {
public:
    static std::string parse(const std::string& requestText, int64_t maxBodyBytes) {
        std::string buffered = requestText;
        auto attempt = doof_http_server::detail::parseRequest(buffered, maxBodyBytes);
        if (attempt.status == doof_http_server::detail::ParseStatus::NeedMore) {
            return "need-more";
        }
        if (attempt.status == doof_http_server::detail::ParseStatus::Error) {
            return "error|" + attempt.error;
        }

        const auto& request = attempt.request;
        const size_t bodySize = request.body ? request.body->size() : 0;
        return "complete|" +
            request.method + "|" +
            request.target + "|" +
            request.version + "|" +
            (request.keepAlive ? "keep-alive" : "close") + "|" +
            std::to_string(bodySize) + "|" +
            std::to_string(buffered.size()) + "|" +
            request.headersText;
    }
};

class NativeHttpTestRequest {
public:
    static std::shared_ptr<NativeHttpTestRequest> start(
        const std::string& host,
        int32_t port,
        const std::string& requestText
    ) {
        auto request = std::shared_ptr<NativeHttpTestRequest>(
            new NativeHttpTestRequest(host, port, requestText)
        );
        request->worker_ = std::thread([request] {
            request->run();
        });
        return request;
    }

    ~NativeHttpTestRequest() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::string wait() {
        if (worker_.joinable()) {
            worker_.join();
        }
        return response_;
    }

    std::shared_ptr<std::vector<uint8_t>> waitBytes() {
        if (worker_.joinable()) {
            worker_.join();
        }
        return std::make_shared<std::vector<uint8_t>>(response_.begin(), response_.end());
    }

private:
    NativeHttpTestRequest(std::string host, int32_t port, std::string requestText)
        : host_(std::move(host)), port_(port), requestText_(std::move(requestText)) {}

    void run() {
        int fd = -1;
        for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
            fd = connectOnce();
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (fd < 0) {
            response_ = "ERROR: failed to connect";
            return;
        }

        size_t offset = 0;
        while (offset < requestText_.size()) {
            const ssize_t written = ::send(fd, requestText_.data() + offset, requestText_.size() - offset, 0);
            if (written > 0) {
                offset += static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            response_ = "ERROR: failed to write request";
            ::close(fd);
            return;
        }

        std::vector<char> buffer(4096);
        while (true) {
            const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (count > 0) {
                response_.append(buffer.data(), static_cast<size_t>(count));
                continue;
            }
            if (count == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            response_ = "ERROR: failed to read response";
            break;
        }
        ::close(fd);
    }

    int connectOnce() const {
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* addresses = nullptr;
        const std::string portText = std::to_string(port_);
        if (::getaddrinfo(host_.c_str(), portText.c_str(), &hints, &addresses) != 0) {
            return -1;
        }

        int fd = -1;
        for (addrinfo* current = addresses; current != nullptr; current = current->ai_next) {
            fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (::connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
                break;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(addresses);
        return fd;
    }

    std::string host_;
    int32_t port_;
    std::string requestText_;
    std::string response_;
    std::thread worker_;
};

class NativeHttpSlowTestRequest {
public:
    static std::shared_ptr<NativeHttpSlowTestRequest> start(
        const std::string& host,
        int32_t port,
        const std::string& firstChunk,
        const std::string& secondChunk,
        int32_t delayMillis
    ) {
        auto request = std::shared_ptr<NativeHttpSlowTestRequest>(
            new NativeHttpSlowTestRequest(host, port, firstChunk, secondChunk, delayMillis)
        );
        request->worker_ = std::thread([request] {
            request->run();
        });
        return request;
    }

    ~NativeHttpSlowTestRequest() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::string wait() {
        if (worker_.joinable()) {
            worker_.join();
        }
        return response_;
    }

private:
    NativeHttpSlowTestRequest(
        std::string host,
        int32_t port,
        std::string firstChunk,
        std::string secondChunk,
        int32_t delayMillis
    )
        : host_(std::move(host)),
          port_(port),
          firstChunk_(std::move(firstChunk)),
          secondChunk_(std::move(secondChunk)),
          delayMillis_(delayMillis) {}

    void run() {
        std::signal(SIGPIPE, SIG_IGN);
        int fd = -1;
        for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
            fd = connectOnce();
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (fd < 0) {
            response_ = "ERROR: failed to connect";
            return;
        }

        if (!writeAll(fd, firstChunk_)) {
            response_ = "ERROR: failed to write first chunk";
            ::close(fd);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMillis_));
        if (!secondChunk_.empty() && !writeAll(fd, secondChunk_)) {
            response_ = "ERROR: failed to write second chunk";
            ::close(fd);
            return;
        }

        std::vector<char> buffer(4096);
        while (true) {
            const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (count > 0) {
                response_.append(buffer.data(), static_cast<size_t>(count));
                continue;
            }
            if (count == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            response_ = "ERROR: failed to read response";
            break;
        }
        ::close(fd);
    }

    bool writeAll(int fd, const std::string& text) const {
        size_t offset = 0;
        while (offset < text.size()) {
            const ssize_t written = ::send(fd, text.data() + offset, text.size() - offset, 0);
            if (written > 0) {
                offset += static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    int connectOnce() const {
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* addresses = nullptr;
        const std::string portText = std::to_string(port_);
        if (::getaddrinfo(host_.c_str(), portText.c_str(), &hints, &addresses) != 0) {
            return -1;
        }

        int fd = -1;
        for (addrinfo* current = addresses; current != nullptr; current = current->ai_next) {
            fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (::connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
                break;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(addresses);
        return fd;
    }

    std::string host_;
    int32_t port_;
    std::string firstChunk_;
    std::string secondChunk_;
    int32_t delayMillis_;
    std::string response_;
    std::thread worker_;
};

class NativeWebSocketTestClient {
public:
    static std::shared_ptr<NativeWebSocketTestClient> startExchangeText(
        const std::string& host,
        int32_t port,
        const std::string& requestText,
        const std::string& text
    ) {
        auto client = std::shared_ptr<NativeWebSocketTestClient>(
            new NativeWebSocketTestClient(host, port, requestText, text, true)
        );
        client->worker_ = std::thread([client] {
            client->response_ = exchangeText(client->host_, client->port_, client->requestText_, client->text_);
        });
        return client;
    }

    static std::shared_ptr<NativeWebSocketTestClient> startHandshakeOnly(
        const std::string& host,
        int32_t port,
        const std::string& requestText
    ) {
        auto client = std::shared_ptr<NativeWebSocketTestClient>(
            new NativeWebSocketTestClient(host, port, requestText, "", false)
        );
        client->worker_ = std::thread([client] {
            client->response_ = handshakeOnly(client->host_, client->port_, client->requestText_);
        });
        return client;
    }

    ~NativeWebSocketTestClient() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::string wait() {
        if (worker_.joinable()) {
            worker_.join();
        }
        return response_;
    }

    static std::string exchangeText(
        const std::string& host,
        int32_t port,
        const std::string& requestText,
        const std::string& text
    ) {
        NativeWebSocketTestClient client(host, port);
        const int fd = client.connectWithRetry();
        if (fd < 0) {
            return "ERROR: failed to connect";
        }
        if (!client.writeAll(fd, requestText)) {
            ::close(fd);
            return "ERROR: failed to write handshake";
        }
        std::string response = client.readUntil(fd, "\r\n\r\n");
        if (response.find("HTTP/1.1 101 Switching Protocols") == std::string::npos) {
            ::close(fd);
            return response;
        }
        if (!client.writeAll(fd, maskedFrame(0x1, std::vector<uint8_t>(text.begin(), text.end())))) {
            ::close(fd);
            return "ERROR: failed to write frame";
        }
        response += client.readServerFrame(fd);
        (void)client.writeAll(fd, maskedFrame(0x8, closePayload(1000, "")));
        response += client.readServerFrame(fd);
        ::close(fd);
        return response;
    }

    static std::string handshakeOnly(
        const std::string& host,
        int32_t port,
        const std::string& requestText
    ) {
        NativeWebSocketTestClient client(host, port);
        const int fd = client.connectWithRetry();
        if (fd < 0) {
            return "ERROR: failed to connect";
        }
        if (!client.writeAll(fd, requestText)) {
            ::close(fd);
            return "ERROR: failed to write handshake";
        }
        std::string response = client.readUntil(fd, "\r\n\r\n");
        ::close(fd);
        return response;
    }

private:
    NativeWebSocketTestClient(std::string host, int32_t port)
        : host_(std::move(host)), port_(port) {}

    NativeWebSocketTestClient(
        std::string host,
        int32_t port,
        std::string requestText,
        std::string text,
        bool exchange
    )
        : host_(std::move(host)),
          port_(port),
          requestText_(std::move(requestText)),
          text_(std::move(text)),
          exchange_(exchange) {}

    static std::vector<uint8_t> closePayload(int32_t code, const std::string& reason) {
        std::vector<uint8_t> payload;
        payload.push_back(static_cast<uint8_t>((code >> 8) & 0xff));
        payload.push_back(static_cast<uint8_t>(code & 0xff));
        payload.insert(payload.end(), reason.begin(), reason.end());
        return payload;
    }

    static std::string maskedFrame(uint8_t opcode, const std::vector<uint8_t>& payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | (opcode & 0x0f)));
        if (payload.size() <= 125) {
            frame.push_back(static_cast<char>(0x80 | payload.size()));
        } else if (payload.size() <= 65535) {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
            frame.push_back(static_cast<char>(payload.size() & 0xff));
        }
        const uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
        for (uint8_t byte : mask) {
            frame.push_back(static_cast<char>(byte));
        }
        for (size_t i = 0; i < payload.size(); ++i) {
            frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        }
        return frame;
    }

    bool writeAll(int fd, const std::string& text) const {
        size_t offset = 0;
        while (offset < text.size()) {
            const ssize_t written = ::send(fd, text.data() + offset, text.size() - offset, 0);
            if (written > 0) {
                offset += static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    std::string readUntil(int fd, const std::string& marker) const {
        std::string response;
        std::vector<char> buffer(1);
        while (response.find(marker) == std::string::npos) {
            const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
            if (count > 0) {
                response.append(buffer.data(), static_cast<size_t>(count));
                continue;
            }
            if (count == 0 || (count < 0 && errno != EINTR)) {
                break;
            }
        }
        return response;
    }

    std::string readServerFrame(int fd) const {
        uint8_t header[2] {};
        if (!readExact(fd, header, 2)) {
            return "frame|error|read-header\n";
        }
        if (header[0] == 'H' && header[1] == 'T') {
            (void)readUntil(fd, "\r\n\r\n");
            return readServerFrame(fd);
        }
        const uint8_t opcode = header[0] & 0x0f;
        uint64_t length = header[1] & 0x7f;
        if (length == 126) {
            uint8_t ext[2] {};
            if (!readExact(fd, ext, 2)) {
                return "frame|error|read-length\n";
            }
            length = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (length == 127) {
            uint8_t ext[8] {};
            if (!readExact(fd, ext, 8)) {
                return "frame|error|read-length\n";
            }
            length = 0;
            for (uint8_t byte : ext) {
                length = (length << 8) | byte;
            }
        }
        std::vector<uint8_t> payload(static_cast<size_t>(length));
        if (!payload.empty() && !readExact(fd, payload.data(), payload.size())) {
            return "frame|error|read-payload\n";
        }
        if (opcode == 0x8) {
            int32_t code = 0;
            std::string reason;
            if (payload.size() >= 2) {
                code = (static_cast<int32_t>(payload[0]) << 8) | payload[1];
                reason.assign(payload.begin() + 2, payload.end());
            }
            return "frame|close|" + std::to_string(code) + "|" + reason + "\n";
        }
        return "frame|" + std::to_string(static_cast<int32_t>(opcode)) + "|" +
            std::string(payload.begin(), payload.end()) + "\n";
    }

    bool readExact(int fd, uint8_t* data, size_t size) const {
        size_t offset = 0;
        while (offset < size) {
            const ssize_t count = ::recv(fd, data + offset, size - offset, 0);
            if (count > 0) {
                offset += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    int connectWithRetry() const {
        int fd = -1;
        for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
            fd = connectOnce();
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        return fd;
    }

    int connectOnce() const {
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* addresses = nullptr;
        const std::string portText = std::to_string(port_);
        if (::getaddrinfo(host_.c_str(), portText.c_str(), &hints, &addresses) != 0) {
            return -1;
        }

        int fd = -1;
        for (addrinfo* current = addresses; current != nullptr; current = current->ai_next) {
            fd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (fd < 0) {
                continue;
            }
            if (::connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
                break;
            }
            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(addresses);
        return fd;
    }

    std::string host_;
    int32_t port_;
    std::string requestText_;
    std::string text_;
    bool exchange_ = false;
    std::string response_;
    std::thread worker_;
};

}  // namespace doof_http_server_test
