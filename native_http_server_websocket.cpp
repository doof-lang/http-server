#include "websocket.hpp"

namespace {

std::shared_ptr<std_::http_server::errors::ServerError> parseServerErrorForWebSocket(const std::string& raw) {
    const size_t separator = raw.find('|');
    if (separator == std::string::npos) {
        return std::make_shared<std_::http_server::errors::ServerError>("server", raw);
    }
    return std::make_shared<std_::http_server::errors::ServerError>(
        raw.substr(0, separator),
        raw.substr(separator + 1)
    );
}

}  // namespace

namespace doof_http_server {

// Returns the inbound read pressure: 0 keeps reading, 1 pauses reads until
// enough posted events have been handled, 2 means the handler's actor
// cannot accept events and the connection should fail.
int32_t NativeWebSocketConnection::deliver(PublicEvent event, bool terminal) {
    EventHandler onEvent;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        onEvent = onEvent_;
        if (terminal) {
            // The posted event keeps its own handler copy; dropping ours
            // breaks the connection -> handler -> connection cycle.
            onEvent_ = {};
            connection_.reset();
        }
        if (!onEvent) {
            return 0;
        }
        ++pendingEvents_;
    }

    auto posted = onEvent.tryPost(event);
    if (doof::is_success(posted)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!terminal && pendingEvents_ >= eventCapacity_) {
            inboundPaused_ = true;
            return 1;
        }
        return 0;
    }

    const std::string error = doof::failure_error(posted);
    if (terminal && error == "full") {
        // Close and error events are never shed for a merely busy owner.
        onEvent.dispatch(std::move(event));
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --pendingEvents_;
    }
    return terminal ? 0 : 2;
}

int32_t NativeWebSocketConnection::emit(
    NativeWebSocketEventKind kind,
    std::string text,
    std::shared_ptr<std::vector<uint8_t>> bytes,
    int32_t code,
    std::string reason,
    bool wasClean,
    std::string error
) {
    std::shared_ptr<std_::http_server::websocket::WebSocketConnection> connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection = connection_;
    }
    if (!connection) {
        return 0;
    }

    PublicEvent publicEvent = std::make_shared<std_::http_server::websocket::WebSocketOpen>(connection);
    switch (kind) {
        case NativeWebSocketEventKind::Text:
            publicEvent = std::make_shared<std_::http_server::websocket::WebSocketText>(connection, std::move(text));
            break;
        case NativeWebSocketEventKind::Binary:
            publicEvent = std::make_shared<std_::http_server::websocket::WebSocketBinary>(
                connection,
                bytes ? std::move(bytes) : std::make_shared<std::vector<uint8_t>>()
            );
            break;
        case NativeWebSocketEventKind::Writable:
            publicEvent = std::make_shared<std_::http_server::websocket::WebSocketWritable>(connection);
            break;
        case NativeWebSocketEventKind::Close:
            publicEvent = std::make_shared<std_::http_server::websocket::WebSocketClose>(
                connection,
                code,
                std::move(reason),
                wasClean
            );
            break;
        case NativeWebSocketEventKind::Error:
            publicEvent = std::make_shared<std_::http_server::websocket::WebSocketError>(
                connection,
                parseServerErrorForWebSocket(error)
            );
            break;
        case NativeWebSocketEventKind::Open:
            break;
    }

    const bool terminal = kind == NativeWebSocketEventKind::Close || kind == NativeWebSocketEventKind::Error;
    return deliver(std::move(publicEvent), terminal);
}

}  // namespace doof_http_server
