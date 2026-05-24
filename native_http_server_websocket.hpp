#pragma once

#include "native_http_server_protocol.hpp"
#include "native_http_server_websocket_frames.hpp"

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
    using EventCallback = std::function<void(std::shared_ptr<NativeWebSocketEvent>)>;
    using Sender = std::function<doof::Result<void, std::string>(
        int32_t opcode,
        const std::shared_ptr<std::vector<uint8_t>>& payload,
        int32_t closeCode,
        const std::string& closeReason
    )>;

    static std::shared_ptr<NativeWebSocketConnection> constructor() {
        return std::make_shared<NativeWebSocketConnection>();
    }

    void setEventCallback(EventCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    void attach(Sender sender) {
        std::lock_guard<std::mutex> lock(mutex_);
        sender_ = std::move(sender);
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

    void markOpen() {
        setState(NativeWebSocketState::Open);
        emit(NativeWebSocketEventKind::Open, "", {}, 0, "", true, "");
    }

    void markError(const std::string& error) {
        setState(NativeWebSocketState::Error);
        emit(NativeWebSocketEventKind::Error, "", {}, 0, "", false, error);
    }

    void markClosed(int32_t code, const std::string& reason, bool wasClean) {
        setState(NativeWebSocketState::Closed);
        emit(NativeWebSocketEventKind::Close, "", {}, code, reason, wasClean, "");
    }

    void emitText(const std::string& text) {
        emit(NativeWebSocketEventKind::Text, text, {}, 0, "", true, "");
    }

    void emitBinary(std::shared_ptr<std::vector<uint8_t>> bytes) {
        emit(NativeWebSocketEventKind::Binary, "", std::move(bytes), 0, "", true, "");
    }

    void emitWritable() {
        emit(NativeWebSocketEventKind::Writable, "", {}, 0, "", true, "");
    }

private:
    void setState(NativeWebSocketState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }

    void emit(
        NativeWebSocketEventKind kind,
        std::string text,
        std::shared_ptr<std::vector<uint8_t>> bytes,
        int32_t code,
        std::string reason,
        bool wasClean,
        std::string error
    ) {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (callback) {
            callback(std::make_shared<NativeWebSocketEvent>(
                kind,
                std::move(text),
                std::move(bytes),
                code,
                std::move(reason),
                wasClean,
                std::move(error)
            ));
        }
    }

    mutable std::mutex mutex_;
    NativeWebSocketState state_ = NativeWebSocketState::Connecting;
    EventCallback callback_;
    Sender sender_;
};

}  // namespace doof_http_server
