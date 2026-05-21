#pragma once

#include "native_http_server_common.hpp"

namespace doof_http_server {
namespace detail {

class ConnectionTransport {
public:
    virtual ~ConnectionTransport() = default;
    virtual int fd() const = 0;
    virtual ssize_t read(char* data, size_t size) = 0;
    virtual ssize_t write(const uint8_t* data, size_t size) = 0;
    virtual void close() = 0;
};

class PlainSocketTransport final : public ConnectionTransport {
public:
    explicit PlainSocketTransport(int fd) : fd_(fd) {}

    ~PlainSocketTransport() override {
        close();
    }

    int fd() const override {
        return fd_;
    }

    ssize_t read(char* data, size_t size) override {
        return ::recv(fd_, data, size, 0);
    }

    ssize_t write(const uint8_t* data, size_t size) override {
        return ::send(fd_, data, size, 0);
    }

    void close() override {
        const int oldFd = fd_;
        fd_ = -1;
        closeSocket(oldFd);
    }

private:
    int fd_;
};

}  // namespace detail
}  // namespace doof_http_server
