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

std::shared_ptr<std::vector<uint8_t>> bytesFromString(const std::string& text) {
    return std::make_shared<std::vector<uint8_t>>(text.begin(), text.end());
}

}  // namespace

namespace doof_http_server {

void NativeWebSocketConnection::attachNativeChannels(
    std::shared_ptr<std_::http_server::websocket::WebSocketConnection> connection,
    std::shared_ptr<doof_event::NativeChannel> eventChannel,
    std::shared_ptr<doof_event::NativeChannel> commandChannel
) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = connection;
        eventChannel_ = eventChannel;
        commandChannel_ = commandChannel;
    }
    if (eventChannel) {
        auto weak = weak_from_this();
        eventChannel->registerNativeSenderReady([weak]() {
            if (auto self = weak.lock()) {
                self->resumeInboundReads();
            }
        });
        eventChannel->registerNativeSenderClosed([weak]() {
            if (auto self = weak.lock()) {
                (void)self->close(std_::http_server::websocket::WEBSOCKET_CLOSE_NORMAL, "");
            }
        });
    }
    if (commandChannel) {
        auto weak = weak_from_this();
        commandChannel->registerNativeReceiverMessage<PublicCommand>(
            [weak](PublicCommand command) {
                if (auto self = weak.lock()) {
                    self->handleCommand(std::move(command));
                }
            }
        );
        commandChannel->registerNativeReceiverClosed([weak]() {
            if (auto self = weak.lock()) {
                (void)self->close(std_::http_server::websocket::WEBSOCKET_CLOSE_NORMAL, "");
            }
        });
    }
}

void NativeWebSocketConnection::handleCommand(PublicCommand command) {
    pauseCommandChannel();
    doof::Result<void, std::string> result = doof::Success<void>{};
    bool waitsForWritable = false;

    if (auto* text = std::get_if<std::shared_ptr<std_::http_server::websocket::WebSocketSendText>>(&command)) {
        result = sendRaw(0x1, bytesFromString((*text)->text), 0, "");
        waitsForWritable = doof::is_success(result);
    } else if (auto* binary = std::get_if<std::shared_ptr<std_::http_server::websocket::WebSocketSendBinary>>(&command)) {
        result = sendRaw(0x2, (*binary)->bytes, 0, "");
        waitsForWritable = doof::is_success(result);
    } else if (std::holds_alternative<std::shared_ptr<std_::http_server::websocket::WebSocketPing>>(command)) {
        result = sendRaw(0x9, std::make_shared<std::vector<uint8_t>>(), 0, "");
        waitsForWritable = doof::is_success(result);
    } else if (auto* closeCommand = std::get_if<std::shared_ptr<std_::http_server::websocket::WebSocketCloseCommand>>(&command)) {
        result = close((*closeCommand)->code, (*closeCommand)->reason);
        waitsForWritable = doof::is_success(result);
    }

    if (doof::is_failure(result)) {
        resumeCommandChannel();
        emitErrorToPublicChannel(doof::failure_error(result));
        return;
    }
    if (!waitsForWritable) {
        resumeCommandChannel();
    }
}

void NativeWebSocketConnection::pauseCommandChannel() {
    std::shared_ptr<doof_event::NativeChannel> commandChannel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commandChannel = commandChannel_;
    }
    if (commandChannel) {
        commandChannel->pauseReceiver();
    }
}

void NativeWebSocketConnection::resumeCommandChannel() {
    std::shared_ptr<doof_event::NativeChannel> commandChannel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commandChannel = commandChannel_;
    }
    if (commandChannel) {
        commandChannel->resumeReceiver();
    }
}

void NativeWebSocketConnection::emitErrorToPublicChannel(const std::string& raw) {
    emitPublicEvent(std::make_shared<std_::http_server::websocket::WebSocketError>(
        publicConnection(),
        parseServerErrorForWebSocket(raw)
    ), false);
}

std::shared_ptr<std_::http_server::websocket::WebSocketConnection> NativeWebSocketConnection::publicConnection() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_;
}

int32_t NativeWebSocketConnection::emitPublicEvent(PublicEvent event, bool keyed) {
    std::shared_ptr<doof_event::NativeChannel> eventChannel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        eventChannel = eventChannel_;
    }
    if (!eventChannel) {
        return 0;
    }
    return eventChannel->trySendMessage(std::move(event), keyed, "websocket:writable");
}

void NativeWebSocketConnection::closePublicChannels() {
    std::shared_ptr<doof_event::NativeChannel> eventChannel;
    std::shared_ptr<doof_event::NativeChannel> commandChannel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        eventChannel = eventChannel_;
        commandChannel = commandChannel_;
        connection_.reset();
    }
    if (commandChannel) {
        commandChannel->tryClose();
    }
    if (eventChannel) {
        eventChannel->tryClose();
    }
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
    auto connection = publicConnection();
    if (!connection) {
        return 0;
    }

    PublicEvent publicEvent = std::make_shared<std_::http_server::websocket::WebSocketOpen>(connection);
    bool keyed = false;
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
            resumeCommandChannel();
            keyed = true;
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

    const int32_t pressure = emitPublicEvent(std::move(publicEvent), keyed);
    if (kind == NativeWebSocketEventKind::Close || kind == NativeWebSocketEventKind::Error) {
        closePublicChannels();
    }
    return pressure;
}

}  // namespace doof_http_server
