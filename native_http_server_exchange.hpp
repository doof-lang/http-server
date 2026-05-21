#pragma once

#include "native_http_server_protocol.hpp"

namespace doof_http_server {

class NativeConnection;

class NativeResponder : public std::enable_shared_from_this<NativeResponder> {
public:
    NativeResponder(std::shared_ptr<NativeConnection> connection, bool requestKeepAlive)
        : connection_(std::move(connection)), requestKeepAlive_(requestKeepAlive) {}

    doof::Result<void, std::string> respond(
        int32_t status,
        const std::string& headersText,
        const std::shared_ptr<std::vector<uint8_t>>& body
    );

private:
    mutable std::mutex mutex_;
    std::shared_ptr<NativeConnection> connection_;
    bool requestKeepAlive_;
    bool completed_ = false;
    bool responseWritten_ = false;
};

class NativeExchange {
public:
    NativeExchange(detail::ParsedRequest request, std::shared_ptr<NativeResponder> responder)
        : request_(std::move(request)), responder_(std::move(responder)) {}

    std::string method() const { return request_.method; }
    std::string target() const { return request_.target; }
    std::string version() const { return request_.version; }
    std::string headersText() const { return request_.headersText; }
    std::shared_ptr<std::vector<uint8_t>> body() const { return request_.body; }
    std::shared_ptr<NativeResponder> responder() const { return responder_; }

private:
    detail::ParsedRequest request_;
    std::shared_ptr<NativeResponder> responder_;
};

}  // namespace doof_http_server
