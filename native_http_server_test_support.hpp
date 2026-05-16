#pragma once

#include "doof_runtime.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace doof_http_server_test {

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

}  // namespace doof_http_server_test
