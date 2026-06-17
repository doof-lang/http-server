#pragma once

#include "native_event.hpp"
#include "native_http_server_protocol.hpp"
#include "native_http_server_websocket_frames.hpp"

#include <variant>

namespace std_::event::index {
template <typename T>
struct ChannelReceiver;
template <typename T>
struct ChannelSender;
}

namespace std_::http_server::websocket {
struct WebSocketBinary;
struct WebSocketClose;
struct WebSocketCloseCommand;
struct WebSocketConnection;
struct WebSocketError;
struct WebSocketOpen;
struct WebSocketPing;
struct WebSocketSendBinary;
struct WebSocketSendText;
struct WebSocketText;
struct WebSocketWritable;
}

namespace doof_http_server {

enum class NativeWebSocketState : int32_t {
    Connecting = 0,
    Open = 1,
    Closing = 2,
    Closed = 3,
    Error = 4,
};

enum class NativeWebSocketEventKind : int32_t {
    Open = 0,
    Text = 1,
    Binary = 2,
    Writable = 3,
    Close = 4,
    Error = 5,
};

class NativeWebSocketEvent {
public:
    NativeWebSocketEvent(
        NativeWebSocketEventKind kind,
        std::string text,
        std::shared_ptr<std::vector<uint8_t>> bytes,
        int32_t code,
        std::string reason,
        bool wasClean,
        std::string error
    )
        : kind_(kind),
          text_(std::move(text)),
          bytes_(std::move(bytes)),
          code_(code),
          reason_(std::move(reason)),
          wasClean_(wasClean),
          error_(std::move(error)) {}

    int32_t kind() const { return static_cast<int32_t>(kind_); }
    std::string text() const { return text_; }
    std::shared_ptr<std::vector<uint8_t>> bytes() const { return bytes_ ? bytes_ : std::make_shared<std::vector<uint8_t>>(); }
    int32_t code() const { return code_; }
    std::string reason() const { return reason_; }
    bool wasClean() const { return wasClean_; }
    std::string error() const { return error_; }

private:
    NativeWebSocketEventKind kind_;
    std::string text_;
    std::shared_ptr<std::vector<uint8_t>> bytes_;
    int32_t code_;
    std::string reason_;
    bool wasClean_;
    std::string error_;
};

class NativeWebSocketConnection : public std::enable_shared_from_this<NativeWebSocketConnection> {
public:
    using PublicEvent = std::variant<
        std::shared_ptr<std_::http_server::websocket::WebSocketOpen>,
        std::shared_ptr<std_::http_server::websocket::WebSocketText>,
        std::shared_ptr<std_::http_server::websocket::WebSocketBinary>,
        std::shared_ptr<std_::http_server::websocket::WebSocketWritable>,
        std::shared_ptr<std_::http_server::websocket::WebSocketClose>,
        std::shared_ptr<std_::http_server::websocket::WebSocketError>
    >;
    using PublicCommand = std::variant<
        std::shared_ptr<std_::http_server::websocket::WebSocketSendText>,
        std::shared_ptr<std_::http_server::websocket::WebSocketSendBinary>,
        std::shared_ptr<std_::http_server::websocket::WebSocketPing>,
        std::shared_ptr<std_::http_server::websocket::WebSocketCloseCommand>
    >;
    using EventSender = std_::event::index::ChannelSender<PublicEvent>;
    using CommandReceiver = std_::event::index::ChannelReceiver<PublicCommand>;
    using ResumeInbound = std::function<void()>;
    using Sender = std::function<doof::Result<void, std::string>(
        int32_t opcode,
        const std::shared_ptr<std::vector<uint8_t>>& payload,
        int32_t closeCode,
        const std::string& closeReason
    )>;

    static std::shared_ptr<NativeWebSocketConnection> constructor() {
        return std::make_shared<NativeWebSocketConnection>();
    }

    ~NativeWebSocketConnection() {
        removeKeepAlive();
    }

    void attachChannels(
        std::shared_ptr<std_::http_server::websocket::WebSocketConnection> connection,
        std::shared_ptr<EventSender> eventSender,
        std::shared_ptr<CommandReceiver> commandReceiver
    );

    void attach(Sender sender) {
        std::lock_guard<std::mutex> lock(mutex_);
        sender_ = std::move(sender);
    }

    void attachResumeInbound(ResumeInbound resumeInbound) {
        std::lock_guard<std::mutex> lock(mutex_);
        resumeInbound_ = std::move(resumeInbound);
    }

    void resumeInboundReads() {
        ResumeInbound resumeInbound;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resumeInbound = resumeInbound_;
        }
        if (resumeInbound) {
            resumeInbound();
        }
    }

    int32_t state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int32_t>(state_);
    }

    doof::Result<void, std::string> sendText(std::string text);
    doof::Result<void, std::string> sendBinary(std::shared_ptr<std::vector<uint8_t>> bytes);
    doof::Result<void, std::string> ping();

    doof::Result<void, std::string> close(int32_t code, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == NativeWebSocketState::Closed || state_ == NativeWebSocketState::Error) {
                return doof::Result<void, std::string>::failure("closed|websocket is closed");
            }
            state_ = NativeWebSocketState::Closing;
        }
        return sendRaw(0x8, std::make_shared<std::vector<uint8_t>>(), code, reason);
    }

    doof::Result<void, std::string> sendRaw(
        int32_t opcode,
        const std::shared_ptr<std::vector<uint8_t>>& payload,
        int32_t closeCode,
        const std::string& closeReason
    ) {
        Sender sender;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!sender_) {
                return doof::Result<void, std::string>::failure("not-open|websocket is not open");
            }
            if (state_ != NativeWebSocketState::Open && !(opcode == 0x8 && state_ == NativeWebSocketState::Closing)) {
                return doof::Result<void, std::string>::failure("not-open|websocket is not open");
            }
            sender = sender_;
        }
        return sender(opcode, payload ? payload : std::make_shared<std::vector<uint8_t>>(), closeCode, closeReason);
    }

    int32_t markOpen() {
        addKeepAlive();
        setState(NativeWebSocketState::Open);
        return emit(NativeWebSocketEventKind::Open, "", {}, 0, "", true, "");
    }

    int32_t markError(const std::string& error) {
        setState(NativeWebSocketState::Error);
        const auto code = emit(NativeWebSocketEventKind::Error, "", {}, 0, "", false, error);
        closeEventChannel();
        removeKeepAlive();
        return code;
    }

    int32_t markClosed(int32_t code, const std::string& reason, bool wasClean) {
        setState(NativeWebSocketState::Closed);
        const auto pressure = emit(NativeWebSocketEventKind::Close, "", {}, code, reason, wasClean, "");
        closeEventChannel();
        removeKeepAlive();
        return pressure;
    }

    int32_t emitText(const std::string& text) {
        return emit(NativeWebSocketEventKind::Text, text, {}, 0, "", true, "");
    }

    int32_t emitBinary(std::shared_ptr<std::vector<uint8_t>> bytes) {
        return emit(NativeWebSocketEventKind::Binary, "", std::move(bytes), 0, "", true, "");
    }

    int32_t emitWritable() {
        return emit(NativeWebSocketEventKind::Writable, "", {}, 0, "", true, "");
    }

private:
    void addKeepAlive() {
        bool shouldAdd = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!countedKeepAlive_) {
                countedKeepAlive_ = true;
                shouldAdd = true;
            }
        }
        if (shouldAdd) {
            doof::detail::ApplicationDomain::shared().add_keep_alive_source(true);
        }
    }

    void removeKeepAlive() {
        bool shouldRemove = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (countedKeepAlive_) {
                countedKeepAlive_ = false;
                shouldRemove = true;
            }
        }
        if (shouldRemove) {
            doof::detail::ApplicationDomain::shared().remove_keep_alive_source(true);
        }
    }

    void setState(NativeWebSocketState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }

    void closeEventChannel() {
        std::shared_ptr<doof_event::NativeChannel> eventChannel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            eventChannel = eventChannel_;
        }
        if (eventChannel) {
            eventChannel->tryClose();
        }
    }

    void handleCommand(PublicCommand command);
    void pauseCommandChannel();
    void resumeCommandChannel();
    void emitErrorToPublicChannel(const std::string& raw);
    std::shared_ptr<std_::http_server::websocket::WebSocketConnection> publicConnection() const;
    int32_t emitPublicEvent(PublicEvent event, bool keyed);
    void closePublicChannels();

    int32_t emit(
        NativeWebSocketEventKind kind,
        std::string text,
        std::shared_ptr<std::vector<uint8_t>> bytes,
        int32_t code,
        std::string reason,
        bool wasClean,
        std::string error
    );

    mutable std::mutex mutex_;
    NativeWebSocketState state_ = NativeWebSocketState::Connecting;
    bool countedKeepAlive_ = false;
    std::shared_ptr<doof_event::NativeChannel> eventChannel_;
    std::shared_ptr<doof_event::NativeChannel> commandChannel_;
    std::shared_ptr<std_::http_server::websocket::WebSocketConnection> connection_;
    ResumeInbound resumeInbound_;
    Sender sender_;
};

void attachWebSocketChannels(
    std::shared_ptr<NativeWebSocketConnection> native,
    std::shared_ptr<std_::http_server::websocket::WebSocketConnection> connection,
    std::shared_ptr<NativeWebSocketConnection::EventSender> eventSender,
    std::shared_ptr<NativeWebSocketConnection::CommandReceiver> commandReceiver
);

}  // namespace doof_http_server
