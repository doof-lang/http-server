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

    doof::Result<void, std::string> enqueueWebSocketUpgrade(
        const detail::ParsedRequest& request,
        std::shared_ptr<NativeWebSocketConnection> websocket,
        const std::string& headersText,
        const std::string& subprotocol
    ) {
        if (!websocket) {
            return doof::Result<void, std::string>::failure("websocket|missing websocket connection");
        }
        auto accept = detail::validateWebSocketHandshake(request);
        if (accept.isFailure()) {
            enqueueImmediateClose(detail::simpleResponseBytes(400, "Bad Request\n"));
            return doof::Result<void, std::string>::failure(accept.error());
        }

        auto bytes = detail::websocketHandshakeBytes(accept.value(), headersText, subprotocol);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || closeAfterWrite_ || websocketMode_) {
                return doof::Result<void, std::string>::failure("disconnected|request is no longer writable");
            }
            websocketMode_ = true;
            awaitingResponse_ = false;
            websocket_ = std::move(websocket);
            writeBuffer_.insert(writeBuffer_.end(), bytes.begin(), bytes.end());
        }

        auto self = shared_from_this();
        websocket_->attach([self](
            int32_t opcode,
            const std::shared_ptr<std::vector<uint8_t>>& payload,
            int32_t closeCode,
            const std::string& closeReason
        ) {
            return self->enqueueWebSocketFrame(opcode, payload, closeCode, closeReason);
        });
        websocket_->markOpen();
        armWriteInterest();
        return doof::Result<void, std::string>::success();
    }

    doof::Result<void, std::string> enqueueWebSocketFrame(
        int32_t opcode,
        const std::shared_ptr<std::vector<uint8_t>>& payload,
        int32_t closeCode,
        const std::string& closeReason
    ) {
        std::vector<uint8_t> framePayload;
        if (opcode == 0x8) {
            if (closeReason.size() > 123) {
                return doof::Result<void, std::string>::failure("invalid-close|websocket close reason exceeds 123 bytes");
            }
            framePayload = detail::closePayload(closeCode, closeReason);
        } else if (payload) {
            framePayload = *payload;
        }
        if ((opcode == 0x9 || opcode == 0xA) && framePayload.size() > 125) {
            return doof::Result<void, std::string>::failure("frame-too-large|websocket control frame exceeds 125 bytes");
        }
        if (opcode != 0x8 && static_cast<int64_t>(framePayload.size()) > maxBodyBytes_) {
            return doof::Result<void, std::string>::failure("message-too-large|websocket message exceeds configured maxBodyBytes");
        }

        auto bytes = detail::websocketFrame(static_cast<uint8_t>(opcode), framePayload);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || !websocketMode_) {
                return doof::Result<void, std::string>::failure("closed|websocket is closed");
            }
            writeBuffer_.insert(writeBuffer_.end(), bytes.begin(), bytes.end());
            closeAfterWrite_ = closeAfterWrite_ || opcode == 0x8;
        }
        armWriteInterest();
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

        if (isWebSocketMode()) {
            processWebSocketFrames();
        } else {
            tryDispatchBufferedRequest();
        }
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
            if (!shouldClose && !websocketMode_) {
                awaitingResponse_ = false;
                lastActivityAt_ = std::chrono::steady_clock::now();
            } else if (!shouldClose) {
                lastActivityAt_ = std::chrono::steady_clock::now();
            }
        }

        if (shouldClose) {
            closeFromReactor();
            return;
        }

        reactor_->updateHandler(fd(), true, false);
        if (isWebSocketMode()) {
            notifyWebSocketWritable();
        } else {
            tryDispatchBufferedRequest();
        }
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
            if (websocketMode_) {
                shouldClose = false;
            } else if (awaitingResponse_) {
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

    bool isWebSocketMode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return websocketMode_;
    }

    void notifyWebSocketWritable() {
        std::shared_ptr<NativeWebSocketConnection> websocket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            websocket = websocket_;
        }
        if (websocket) {
            websocket->emitWritable();
        }
    }

    void processWebSocketFrames() {
        while (true) {
            std::vector<uint8_t> payload;
            uint8_t opcode = 0;
            bool fin = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_ || !websocketMode_) {
                    return;
                }
                const auto parsed = parseOneWebSocketFrameLocked(fin, opcode, payload);
                if (parsed == FrameParseResult::NeedMore) {
                    return;
                }
                if (parsed == FrameParseResult::Error) {
                    armWriteInterest();
                    return;
                }
            }
            handleWebSocketFrame(fin, opcode, std::move(payload));
            if (shouldCloseAfterWrite()) {
                armWriteInterest();
                return;
            }
        }
    }

    bool shouldCloseAfterWrite() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closeAfterWrite_;
    }

    enum class FrameParseResult {
        NeedMore,
        Complete,
        Error,
    };

    FrameParseResult parseOneWebSocketFrameLocked(bool& fin, uint8_t& opcode, std::vector<uint8_t>& payload) {
        if (readBuffer_.size() < 2) {
            return FrameParseResult::NeedMore;
        }
        const auto b0 = static_cast<uint8_t>(readBuffer_[0]);
        const auto b1 = static_cast<uint8_t>(readBuffer_[1]);
        fin = (b0 & 0x80) != 0;
        opcode = b0 & 0x0f;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t length = b1 & 0x7f;
        size_t offset = 2;
        if (!masked) {
            protocolErrorLocked("protocol-error|websocket client frames must be masked");
            return FrameParseResult::Error;
        }
        if ((b0 & 0x70) != 0) {
            protocolErrorLocked("protocol-error|websocket reserved bits are not supported");
            return FrameParseResult::Error;
        }
        if (length == 126) {
            if (readBuffer_.size() < offset + 2) {
                return FrameParseResult::NeedMore;
            }
            length = (static_cast<uint64_t>(static_cast<uint8_t>(readBuffer_[offset])) << 8) |
                     static_cast<uint64_t>(static_cast<uint8_t>(readBuffer_[offset + 1]));
            offset += 2;
        } else if (length == 127) {
            if (readBuffer_.size() < offset + 8) {
                return FrameParseResult::NeedMore;
            }
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | static_cast<uint64_t>(static_cast<uint8_t>(readBuffer_[offset + static_cast<size_t>(i)]));
            }
            offset += 8;
        }
        if (length > static_cast<uint64_t>(maxBodyBytes_)) {
            protocolErrorLocked("message-too-large|websocket message exceeds configured maxBodyBytes", 1009);
            return FrameParseResult::Error;
        }
        if (opcode >= 0x8 && (!fin || length > 125)) {
            protocolErrorLocked("protocol-error|invalid websocket control frame");
            return FrameParseResult::Error;
        }
        if (readBuffer_.size() < offset + 4 + static_cast<size_t>(length)) {
            return FrameParseResult::NeedMore;
        }

        uint8_t mask[4] = {
            static_cast<uint8_t>(readBuffer_[offset]),
            static_cast<uint8_t>(readBuffer_[offset + 1]),
            static_cast<uint8_t>(readBuffer_[offset + 2]),
            static_cast<uint8_t>(readBuffer_[offset + 3]),
        };
        offset += 4;
        payload.resize(static_cast<size_t>(length));
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>(readBuffer_[offset + i]) ^ mask[i % 4];
        }
        readBuffer_.erase(0, offset + payload.size());
        return FrameParseResult::Complete;
    }

    void handleWebSocketFrame(bool fin, uint8_t opcode, std::vector<uint8_t> payload) {
        std::shared_ptr<NativeWebSocketConnection> websocket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            websocket = websocket_;
        }
        if (!websocket) {
            return;
        }

        if (opcode == 0x8) {
            int32_t code = 1000;
            std::string reason;
            if (payload.size() == 1) {
                closeWithProtocolError("protocol-error|invalid websocket close payload");
                return;
            }
            if (payload.size() >= 2) {
                code = (static_cast<int32_t>(payload[0]) << 8) | static_cast<int32_t>(payload[1]);
                std::vector<uint8_t> reasonBytes(payload.begin() + 2, payload.end());
                if (!detail::isValidUtf8(reasonBytes)) {
                    closeWithProtocolError("invalid-payload|websocket close reason is not valid UTF-8", 1007);
                    return;
                }
                reason.assign(reasonBytes.begin(), reasonBytes.end());
            }
            (void)enqueueWebSocketFrame(0x8, std::make_shared<std::vector<uint8_t>>(), code, reason);
            markWebSocketClosed(code, reason, true);
            return;
        }

        if (opcode == 0x9) {
            (void)enqueueWebSocketFrame(0xA, std::make_shared<std::vector<uint8_t>>(payload), 0, "");
            return;
        }
        if (opcode == 0xA) {
            return;
        }

        if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
            handleDataFrame(fin, opcode, std::move(payload), websocket);
            return;
        }

        closeWithProtocolError("protocol-error|unsupported websocket opcode");
    }

    void handleDataFrame(
        bool fin,
        uint8_t opcode,
        std::vector<uint8_t> payload,
        const std::shared_ptr<NativeWebSocketConnection>& websocket
    ) {
        uint8_t completeOpcode = opcode;
        std::vector<uint8_t> completePayload;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (opcode == 0x0) {
                if (fragmentOpcode_ == 0) {
                    protocolErrorLocked("protocol-error|unexpected websocket continuation frame");
                    return;
                }
                if (fragmentBuffer_.size() + payload.size() > static_cast<size_t>(maxBodyBytes_)) {
                    protocolErrorLocked("message-too-large|websocket message exceeds configured maxBodyBytes", 1009);
                    return;
                }
                fragmentBuffer_.insert(fragmentBuffer_.end(), payload.begin(), payload.end());
                if (!fin) {
                    return;
                }
                completeOpcode = fragmentOpcode_;
                completePayload.swap(fragmentBuffer_);
                fragmentOpcode_ = 0;
            } else {
                if (fragmentOpcode_ != 0) {
                    protocolErrorLocked("protocol-error|new websocket message before continuation completed");
                    return;
                }
                if (!fin) {
                    fragmentOpcode_ = opcode;
                    fragmentBuffer_ = std::move(payload);
                    return;
                }
                completePayload = std::move(payload);
            }
        }

        if (completeOpcode == 0x1) {
            if (!detail::isValidUtf8(completePayload)) {
                closeWithProtocolError("invalid-payload|websocket text is not valid UTF-8", 1007);
                return;
            }
            websocket->emitText(std::string(completePayload.begin(), completePayload.end()));
        } else {
            websocket->emitBinary(std::make_shared<std::vector<uint8_t>>(std::move(completePayload)));
        }
    }

    void protocolErrorLocked(const std::string& message, int32_t code = 1002) {
        auto bytes = detail::websocketFrame(0x8, detail::closePayload(code, ""));
        writeBuffer_.insert(writeBuffer_.end(), bytes.begin(), bytes.end());
        closeAfterWrite_ = true;
        if (websocket_) {
            websocket_->markError(message);
        }
    }

    void closeWithProtocolError(const std::string& message, int32_t code = 1002) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            protocolErrorLocked(message, code);
        }
        armWriteInterest();
    }

    void markWebSocketClosed(int32_t code, const std::string& reason, bool wasClean) {
        std::shared_ptr<NativeWebSocketConnection> websocket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (websocketCloseEmitted_) {
                return;
            }
            websocketCloseEmitted_ = true;
            websocket = websocket_;
        }
        if (websocket) {
            websocket->markClosed(code, reason, wasClean);
        }
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

        auto responder = std::make_shared<NativeResponder>(shared_from_this(), attempt.request);
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
        bool wasWebSocket = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            wasWebSocket = websocketMode_;
            transport = transport_;
            oldFd = transport ? transport->fd() : -1;
        }
        if (transport) {
            transport->close();
        }
        if (wasWebSocket) {
            markWebSocketClosed(1006, "", false);
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
    bool websocketMode_ = false;
    bool websocketCloseEmitted_ = false;
    bool closeAfterWrite_ = false;
    int64_t maxBodyBytes_;
    int32_t idleTimeoutMillis_;
    int32_t responseTimeoutMillis_;
    int32_t maxRequestsPerConnection_;
    int32_t requestsServed_ = 0;
    std::string readBuffer_;
    std::vector<uint8_t> writeBuffer_;
    std::vector<uint8_t> fragmentBuffer_;
    size_t writeOffset_ = 0;
    uint8_t fragmentOpcode_ = 0;
    std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest_;
    std::shared_ptr<detail::Reactor> reactor_;
    std::shared_ptr<NativeWebSocketConnection> websocket_;
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

    auto result = connection->enqueueResponse(status, headersText, body, request_.keepAlive);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        responseWritten_ = result.isSuccess();
        connection_.reset();
    }
    return result;
}

inline void NativeResponder::upgradeWebSocket(
    std::shared_ptr<NativeWebSocketConnection> websocket,
    const std::string& headersText,
    const std::string& subprotocol,
    NativeWebSocketConnection::EventCallback callback
) {
    if (websocket) {
        websocket->setEventCallback(std::move(callback));
    }

    std::shared_ptr<NativeConnection> connection;
    detail::ParsedRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_) {
            if (websocket) {
                websocket->markError(
                    responseWritten_ ? "already-responded|request has already been responded to"
                                     : "disconnected|request is no longer writable"
                );
            }
            return;
        }
        completed_ = true;
        connection = connection_;
        request = request_;
    }

    if (!connection) {
        if (websocket) {
            websocket->markError("disconnected|request is no longer writable");
        }
        return;
    }

    auto result = connection->enqueueWebSocketUpgrade(request, websocket, headersText, subprotocol);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        responseWritten_ = result.isSuccess();
        connection_.reset();
    }
    if (result.isFailure() && websocket) {
        websocket->markError(result.error());
    }
}

}  // namespace doof_http_server
