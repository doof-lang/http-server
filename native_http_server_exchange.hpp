#pragma once

#include "native_http_server_websocket.hpp"

namespace doof_http_server {

class NativeConnection;

class NativeResponder : public std::enable_shared_from_this<NativeResponder> {
public:
    NativeResponder(std::shared_ptr<NativeConnection> connection, detail::ParsedRequest request)
        : connection_(std::move(connection)), request_(std::move(request)) {}

    doof::Result<void, std::string> respond(
        const std::string& responseText,
        const std::shared_ptr<std::vector<uint8_t>>& body,
        bool keepAlive
    );

    doof::Result<void, std::string> beginStreamResponse(
        const std::string& responseText,
        bool keepAlive
    );

    doof::Result<void, std::string> writeStreamBytes(
        const std::shared_ptr<std::vector<uint8_t>>& bytes
    );

    doof::Result<void, std::string> endStreamResponse(
        const std::shared_ptr<std::vector<uint8_t>>& bytes
    );

    void upgradeToWebSocket(
        std::shared_ptr<NativeWebSocketConnection> websocket,
        const std::string& responseText
    );

private:
    mutable std::mutex mutex_;
    std::shared_ptr<NativeConnection> connection_;
    detail::ParsedRequest request_;
    bool completed_ = false;
    bool streamingResponse_ = false;
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
    bool keepAlive() const { return request_.keepAlive; }
    std::shared_ptr<NativeResponder> responder() const { return responder_; }

private:
    detail::ParsedRequest request_;
    std::shared_ptr<NativeResponder> responder_;
};

namespace detail {

using ExchangeHandler = doof::callback<void(std::shared_ptr<NativeExchange>)>;

// Hands each parsed request to one of the server's handlers, in round-robin
// order. Reactor threads post straight to the handler's owning actor, so a
// pool of worker actors receives requests without a dispatcher in between.
class RequestDispatcher {
public:
    explicit RequestDispatcher(ExchangeHandler handler)
        : handlers_(std::make_shared<const std::vector<ExchangeHandler>>(1, std::move(handler))) {}

    void setHandlers(std::vector<ExchangeHandler> handlers) {
        auto replacement = std::make_shared<const std::vector<ExchangeHandler>>(std::move(handlers));
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_ = std::move(replacement);
    }

    size_t handlerCount() const {
        return snapshot()->size();
    }

    // Offers the request to each handler at most once, starting with the next
    // one in turn, and skips handlers whose mailbox is full. Returns false when
    // every handler refused it.
    bool dispatch(const std::shared_ptr<NativeExchange>& exchange) {
        const auto handlers = snapshot();
        const size_t count = handlers->size();
        const size_t start = static_cast<size_t>(next_.fetch_add(1, std::memory_order_relaxed));
        for (size_t offset = 0; offset < count; ++offset) {
#if defined(DOOF_RUNTIME_HAS_CALLBACK_TRY_DISPATCH)
            // Nothing waits on the handler's result, so skip its promise.
            if ((*handlers)[(start + offset) % count].tryDispatch(exchange) == doof::detail::TryEnqueueResult::Accepted) {
                return true;
            }
#else
            if (doof::is_success((*handlers)[(start + offset) % count].tryPost(exchange))) {
                return true;
            }
#endif
        }
        shed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    int64_t shedCount() const {
        return shed_.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<const std::vector<ExchangeHandler>> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handlers_;
    }

    mutable std::mutex mutex_;
    std::shared_ptr<const std::vector<ExchangeHandler>> handlers_;
    std::atomic<uint64_t> next_ {0};
    std::atomic<int64_t> shed_ {0};
};

}  // namespace detail

}  // namespace doof_http_server
