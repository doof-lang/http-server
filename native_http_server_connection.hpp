#pragma once

#include "native_http_server_exchange.hpp"
#include "native_http_server_reactor.hpp"
#include "native_http_server_transport.hpp"

namespace doof_http_server {

class NativeConnection : public detail::ReactorHandler, public std::enable_shared_from_this<NativeConnection> {
public:
    NativeConnection(
        std::shared_ptr<detail::ConnectionTransport> transport,
        int64_t maxBodyBytes,
        int32_t idleTimeoutMillis,
        int32_t responseTimeoutMillis,
        int32_t maxRequestsPerConnection,
        std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest,
        std::shared_ptr<detail::Reactor> reactor
    )
        : transport_(std::move(transport)),
          maxBodyBytes_(maxBodyBytes),
          idleTimeoutMillis_(idleTimeoutMillis),
          responseTimeoutMillis_(responseTimeoutMillis),
          maxRequestsPerConnection_(maxRequestsPerConnection),
          onRequest_(std::move(onRequest)),
          reactor_(std::move(reactor)),
          lastActivityAt_(std::chrono::steady_clock::now()) {}

    ~NativeConnection() {
        closeFromServer();
    }

    int fd() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return transport_ ? transport_->fd() : -1;
    }

    doof::Result<void, std::string> enqueueResponse(
        int32_t status,
        const std::string& headersText,
        const std::shared_ptr<std::vector<uint8_t>>& body,
        bool requestKeepAlive
    ) {
        const bool keepAlive = requestKeepAlive && !detail::responseRequestsClose(headersText);
        auto bytes = detail::responseBytes(status, headersText, body, keepAlive);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || closeAfterWrite_) {
                return doof::Result<void, std::string>::failure("disconnected|request is no longer writable");
            }
            writeBuffer_.insert(writeBuffer_.end(), bytes.begin(), bytes.end());
            closeAfterWrite_ = closeAfterWrite_ || !keepAlive;
        }

        const auto self = shared_from_this();
        if (!reactor_->post([self] {
            self->armWriteInterest();
        })) {
            closeFromServer();
            return doof::Result<void, std::string>::failure("closed|server is no longer accepting work");
        }
        return doof::Result<void, std::string>::success();
    }

    void onReadable() override {
        std::vector<char> buffer(4096);
        while (true) {
            std::shared_ptr<detail::ConnectionTransport> transport;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_) {
                    return;
                }
                transport = transport_;
            }

            const ssize_t readCount = transport->read(buffer.data(), buffer.size());
            if (readCount > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                readBuffer_.append(buffer.data(), static_cast<size_t>(readCount));
                lastActivityAt_ = std::chrono::steady_clock::now();
                continue;
            }
            if (readCount == 0) {
                closeFromReactor();
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            closeFromReactor();
            return;
        }

        tryDispatchBufferedRequest();
    }

    void onWritable() override {
        while (true) {
            std::shared_ptr<detail::ConnectionTransport> transport;
            const uint8_t* data = nullptr;
            size_t remaining = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_) {
                    return;
                }
                transport = transport_;
                if (writeOffset_ >= writeBuffer_.size()) {
                    break;
                }
                data = writeBuffer_.data() + writeOffset_;
                remaining = writeBuffer_.size() - writeOffset_;
            }

            const ssize_t written = transport->write(data, remaining);
            if (written > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                writeOffset_ += static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            closeFromReactor();
            return;
        }

        bool shouldClose = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            writeBuffer_.clear();
            writeOffset_ = 0;
            shouldClose = closeAfterWrite_;
            if (!shouldClose) {
                awaitingResponse_ = false;
                lastActivityAt_ = std::chrono::steady_clock::now();
            }
        }

        if (shouldClose) {
            closeFromReactor();
            return;
        }

        reactor_->updateHandler(fd(), true, false);
        tryDispatchBufferedRequest();
    }

    void closeFromServer() {
        closeCommon(false);
    }

    void onTimer() override {
        bool shouldClose = false;
        bool shouldTimeoutResponse = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || !writeBuffer_.empty()) {
                return;
            }
            const auto idleFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lastActivityAt_
            );
            if (awaitingResponse_) {
                shouldTimeoutResponse = responseTimeoutMillis_ > 0 &&
                    idleFor.count() >= responseTimeoutMillis_;
            } else {
                shouldClose = idleTimeoutMillis_ > 0 && idleFor.count() >= idleTimeoutMillis_;
            }
        }
        if (shouldTimeoutResponse) {
            enqueueImmediateClose(detail::simpleResponseBytes(504, "Gateway Timeout\n"));
            return;
        }
        if (shouldClose) {
            closeFromReactor();
        }
    }

private:
    void armWriteInterest() {
        int currentFd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            currentFd = transport_ ? transport_->fd() : -1;
        }
        reactor_->updateHandler(currentFd, true, true);
        onWritable();
    }

    void tryDispatchBufferedRequest() {
        detail::ParseAttempt attempt;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || awaitingResponse_) {
                return;
            }
            attempt = detail::parseRequest(readBuffer_, maxBodyBytes_);
            if (attempt.status == detail::ParseStatus::Complete) {
                awaitingResponse_ = true;
                lastActivityAt_ = std::chrono::steady_clock::now();
                ++requestsServed_;
                if (maxRequestsPerConnection_ > 0 &&
                    requestsServed_ >= maxRequestsPerConnection_) {
                    attempt.request.keepAlive = false;
                }
            }
        }

        if (attempt.status == detail::ParseStatus::NeedMore) {
            return;
        }
        if (attempt.status == detail::ParseStatus::Error) {
            const std::string& raw = attempt.error;
            const size_t separator = raw.find('|');
            const std::string kind = separator == std::string::npos ? raw : raw.substr(0, separator);
            const int32_t status = kind == "body-too-large" ? 413 :
                                   kind == "unsupported-transfer-encoding" ? 501 : 400;
            enqueueImmediateClose(detail::simpleResponseBytes(status, detail::statusText(status) + "\n"));
            return;
        }

        auto responder = std::make_shared<NativeResponder>(shared_from_this(), attempt.request.keepAlive);
        auto exchange = std::make_shared<NativeExchange>(std::move(attempt.request), responder);
        const int32_t disposition = onRequest_(std::move(exchange));
        if (disposition != 0) {
            enqueueImmediateClose(detail::simpleResponseBytes(503, "Service Unavailable\n"));
        }
    }

    void enqueueImmediateClose(std::vector<uint8_t> bytes) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            writeBuffer_.insert(writeBuffer_.end(), bytes.begin(), bytes.end());
            closeAfterWrite_ = true;
        }
        armWriteInterest();
    }

    void closeFromReactor() {
        closeCommon(true);
    }

    void closeCommon(bool onReactorThread) {
        std::shared_ptr<detail::ConnectionTransport> transport;
        int oldFd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            transport = transport_;
            oldFd = transport ? transport->fd() : -1;
        }
        if (transport) {
            transport->close();
        }
        if (onReactorThread) {
            reactor_->removeHandler(oldFd);
            return;
        }
        auto reactor = reactor_;
        (void)reactor_->post([reactor, oldFd] {
            reactor->removeHandler(oldFd);
        });
    }

    mutable std::mutex mutex_;
    std::shared_ptr<detail::ConnectionTransport> transport_;
    bool closed_ = false;
    bool awaitingResponse_ = false;
    bool closeAfterWrite_ = false;
    int64_t maxBodyBytes_;
    int32_t idleTimeoutMillis_;
    int32_t responseTimeoutMillis_;
    int32_t maxRequestsPerConnection_;
    int32_t requestsServed_ = 0;
    std::string readBuffer_;
    std::vector<uint8_t> writeBuffer_;
    size_t writeOffset_ = 0;
    std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest_;
    std::shared_ptr<detail::Reactor> reactor_;
    std::chrono::steady_clock::time_point lastActivityAt_;
};

inline doof::Result<void, std::string> NativeResponder::respond(
    int32_t status,
    const std::string& headersText,
    const std::shared_ptr<std::vector<uint8_t>>& body
) {
    std::shared_ptr<NativeConnection> connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_) {
            return doof::Result<void, std::string>::failure(
                responseWritten_ ? "already-responded|request has already been responded to"
                                 : "disconnected|request is no longer writable"
            );
        }
        completed_ = true;
        connection = connection_;
    }

    if (!connection) {
        return doof::Result<void, std::string>::failure("disconnected|request is no longer writable");
    }

    auto result = connection->enqueueResponse(status, headersText, body, requestKeepAlive_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        responseWritten_ = result.isSuccess();
        connection_.reset();
    }
    return result;
}

}  // namespace doof_http_server
