#pragma once

#include "native_http_server_websocket.hpp"

namespace doof_http_server {
namespace detail {

class WebSocketSession {
public:
    using SendFrame = std::function<doof::Result<void, std::string>(
        int32_t opcode,
        const std::shared_ptr<std::vector<uint8_t>>& payload,
        int32_t closeCode,
        const std::string& closeReason
    )>;
    using ProtocolClose = std::function<void(const std::string& message, int32_t code)>;
    using MarkClosed = std::function<void(int32_t code, const std::string& reason, bool wasClean)>;

    WebSocketSession(
        std::shared_ptr<NativeWebSocketConnection> websocket,
        int64_t maxBodyBytes,
        SendFrame sendFrame,
        ProtocolClose protocolClose,
        MarkClosed markClosed
    )
        : websocket_(std::move(websocket)),
          maxBodyBytes_(maxBodyBytes),
          sendFrame_(std::move(sendFrame)),
          protocolClose_(std::move(protocolClose)),
          markClosed_(std::move(markClosed)) {}

    std::shared_ptr<NativeWebSocketConnection> connection() const {
        return websocket_;
    }

    void notifyWritable() {
        if (websocket_) {
            websocket_->emitWritable();
        }
    }

    void handleFrame(bool fin, uint8_t opcode, std::vector<uint8_t> payload) {
        if (!websocket_) {
            return;
        }

        if (opcode == 0x8) {
            handleClose(std::move(payload));
            return;
        }
        if (opcode == 0x9) {
            (void)sendFrame_(0xA, std::make_shared<std::vector<uint8_t>>(payload), 0, "");
            return;
        }
        if (opcode == 0xA) {
            return;
        }
        if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
            handleDataFrame(fin, opcode, std::move(payload));
            return;
        }

        protocolClose_("protocol-error|unsupported websocket opcode", 1002);
    }

private:
    void handleClose(std::vector<uint8_t> payload) {
        int32_t code = 1000;
        std::string reason;
        if (payload.size() == 1) {
            protocolClose_("protocol-error|invalid websocket close payload", 1002);
            return;
        }
        if (payload.size() >= 2) {
            code = (static_cast<int32_t>(payload[0]) << 8) | static_cast<int32_t>(payload[1]);
            std::vector<uint8_t> reasonBytes(payload.begin() + 2, payload.end());
            if (!isValidUtf8(reasonBytes)) {
                protocolClose_("invalid-payload|websocket close reason is not valid UTF-8", 1007);
                return;
            }
            reason.assign(reasonBytes.begin(), reasonBytes.end());
        }
        (void)sendFrame_(0x8, std::make_shared<std::vector<uint8_t>>(), code, reason);
        markClosed_(code, reason, true);
    }

    void handleDataFrame(bool fin, uint8_t opcode, std::vector<uint8_t> payload) {
        uint8_t completeOpcode = opcode;
        std::vector<uint8_t> completePayload;

        if (opcode == 0x0) {
            if (fragmentOpcode_ == 0) {
                protocolClose_("protocol-error|unexpected websocket continuation frame", 1002);
                return;
            }
            if (fragmentBuffer_.size() + payload.size() > static_cast<size_t>(maxBodyBytes_)) {
                protocolClose_("message-too-large|websocket message exceeds configured maxBodyBytes", 1009);
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
                protocolClose_("protocol-error|new websocket message before continuation completed", 1002);
                return;
            }
            if (!fin) {
                fragmentOpcode_ = opcode;
                fragmentBuffer_ = std::move(payload);
                return;
            }
            completePayload = std::move(payload);
        }

        if (completeOpcode == 0x1) {
            if (!isValidUtf8(completePayload)) {
                protocolClose_("invalid-payload|websocket text is not valid UTF-8", 1007);
                return;
            }
            websocket_->emitText(std::string(completePayload.begin(), completePayload.end()));
        } else {
            websocket_->emitBinary(std::make_shared<std::vector<uint8_t>>(std::move(completePayload)));
        }
    }

    std::shared_ptr<NativeWebSocketConnection> websocket_;
    int64_t maxBodyBytes_;
    SendFrame sendFrame_;
    ProtocolClose protocolClose_;
    MarkClosed markClosed_;
    std::vector<uint8_t> fragmentBuffer_;
    uint8_t fragmentOpcode_ = 0;
};

}  // namespace detail
}  // namespace doof_http_server
