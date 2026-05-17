#pragma once

#include "doof_runtime.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif

namespace doof_http_server {

namespace detail {

inline void ignoreSigpipe() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::signal(SIGPIPE, SIG_IGN);
    });
}

inline std::string errnoMessage(const std::string& action) {
    return action + ": " + std::strerror(errno);
}

inline std::string statusText(int32_t status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 418: return "I'm a teapot";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "HTTP " + std::to_string(status);
    }
}

inline void closeSocket(int fd) {
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

inline std::string trim(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

inline std::string toLower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

inline bool parseContentLength(const std::string& text, int64_t& out) {
    if (text.empty()) {
        return false;
    }
    int64_t value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        if (value > (INT64_MAX - (ch - '0')) / 10) {
            return false;
        }
        value = value * 10 + static_cast<int64_t>(ch - '0');
    }
    out = value;
    return true;
}

struct ParsedRequest {
    std::string method;
    std::string target;
    std::string version;
    std::string headersText;
    std::shared_ptr<std::vector<uint8_t>> body;
    bool keepAlive = false;
};

inline bool headerValueHasToken(std::string value, std::string_view wanted) {
    value = toLower(std::move(value));
    size_t start = 0;
    while (start <= value.size()) {
        const size_t separator = value.find(',', start);
        const size_t end = separator == std::string::npos ? value.size() : separator;
        if (trim(value.substr(start, end - start)) == wanted) {
            return true;
        }
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    return false;
}

inline bool requestShouldKeepAlive(
    const std::string& version,
    bool connectionClose,
    bool connectionKeepAlive
) {
    if (connectionClose) {
        return false;
    }
    if (version == "HTTP/1.1") {
        return true;
    }
    if (version == "HTTP/1.0") {
        return connectionKeepAlive;
    }
    return false;
}

inline bool responseRequestsClose(const std::string& headersText) {
    std::istringstream lines(headersText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        if (toLower(line.substr(0, separator)) == "connection" &&
            headerValueHasToken(trim(line.substr(separator + 1)), "close")) {
            return true;
        }
    }
    return false;
}

inline std::string normalizedResponseHeaders(
    const std::string& headersText,
    size_t bodySize,
    bool keepAlive
) {
    std::string normalized;
    std::istringstream lines(headersText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        const std::string lowerName = toLower(line.substr(0, separator));
        if (lowerName == "content-length" || lowerName == "connection") {
            continue;
        }
        normalized += line + "\r\n";
    }
    normalized += "Content-Length: " + std::to_string(bodySize) + "\r\n";
    normalized += std::string("Connection: ") + (keepAlive ? "keep-alive" : "close") + "\r\n";
    return normalized;
}

enum class ParseStatus {
    NeedMore,
    Complete,
    Error,
};

struct ParseAttempt {
    ParseStatus status = ParseStatus::NeedMore;
    ParsedRequest request;
    std::string error;
};

inline ParseAttempt parseRequest(std::string& buffered, int64_t maxBodyBytes) {
    constexpr size_t maxHeaderBytes = 64 * 1024;
    const size_t headerEnd = buffered.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        if (buffered.size() > maxHeaderBytes) {
            return ParseAttempt {
                ParseStatus::Error,
                ParsedRequest {},
                "headers-too-large|HTTP request headers exceed 65536 bytes",
            };
        }
        return ParseAttempt {};
    }

    const std::string headerBlock = buffered.substr(0, headerEnd);
    std::istringstream lines(headerBlock);
    std::string requestLine;
    if (!std::getline(lines, requestLine)) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|missing request line" };
    }
    if (!requestLine.empty() && requestLine.back() == '\r') {
        requestLine.pop_back();
    }

    std::istringstream requestLineStream(requestLine);
    ParsedRequest parsed;
    if (!(requestLineStream >> parsed.method >> parsed.target >> parsed.version)) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid request line" };
    }
    if (parsed.version.rfind("HTTP/", 0) != 0) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid HTTP version" };
    }

    int64_t contentLength = 0;
    bool sawContentLength = false;
    bool connectionClose = false;
    bool connectionKeepAlive = false;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const size_t separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid header line" };
        }

        const std::string name = line.substr(0, separator);
        const std::string value = trim(line.substr(separator + 1));
        const std::string lowerName = toLower(name);

        if (lowerName == "content-length") {
            if (sawContentLength) {
                return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|duplicate Content-Length header" };
            }
            if (!parseContentLength(value, contentLength)) {
                return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "malformed-request|invalid Content-Length header" };
            }
            sawContentLength = true;
        }
        if (lowerName == "transfer-encoding" && toLower(value) != "identity") {
            return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "unsupported-transfer-encoding|chunked transfer encoding is not supported yet" };
        }
        if (lowerName == "connection") {
            connectionClose = connectionClose || headerValueHasToken(value, "close");
            connectionKeepAlive = connectionKeepAlive || headerValueHasToken(value, "keep-alive");
        }

        parsed.headersText += name + ": " + value + "\r\n";
    }

    if (contentLength < 0 || contentLength > maxBodyBytes) {
        return ParseAttempt { ParseStatus::Error, ParsedRequest {}, "body-too-large|request body exceeds configured maxBodyBytes" };
    }

    const size_t bodyStart = headerEnd + 4;
    const size_t totalSize = bodyStart + static_cast<size_t>(contentLength);
    if (buffered.size() < totalSize) {
        return ParseAttempt {};
    }

    auto body = std::make_shared<std::vector<uint8_t>>();
    body->reserve(static_cast<size_t>(contentLength));
    if (contentLength > 0) {
        body->insert(body->end(), buffered.begin() + static_cast<std::ptrdiff_t>(bodyStart),
                     buffered.begin() + static_cast<std::ptrdiff_t>(totalSize));
    }

    parsed.body = std::move(body);
    parsed.keepAlive = requestShouldKeepAlive(parsed.version, connectionClose, connectionKeepAlive);
    buffered.erase(0, totalSize);
    return ParseAttempt { ParseStatus::Complete, std::move(parsed), "" };
}

inline doof::Result<void, std::string> setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return doof::Result<void, std::string>::failure(errnoMessage("failed to read socket flags"));
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return doof::Result<void, std::string>::failure(errnoMessage("failed to set socket nonblocking"));
    }
    return doof::Result<void, std::string>::success();
}

inline std::vector<uint8_t> responseBytes(
    int32_t status,
    const std::string& headersText,
    const std::shared_ptr<std::vector<uint8_t>>& body,
    bool keepAlive
) {
    const auto safeBody = body ? body : std::make_shared<std::vector<uint8_t>>();
    const std::string head =
        "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n" +
        normalizedResponseHeaders(headersText, safeBody->size(), keepAlive) +
        "\r\n";
    std::vector<uint8_t> bytes;
    bytes.reserve(head.size() + safeBody->size());
    bytes.insert(bytes.end(), head.begin(), head.end());
    bytes.insert(bytes.end(), safeBody->begin(), safeBody->end());
    return bytes;
}

inline std::vector<uint8_t> simpleResponseBytes(int32_t status, const std::string& body) {
    return responseBytes(
        status,
        "Content-Type: text/plain; charset=utf-8\r\n",
        std::make_shared<std::vector<uint8_t>>(body.begin(), body.end()),
        false
    );
}

class ReactorHandler {
public:
    virtual ~ReactorHandler() = default;
    virtual int fd() const = 0;
    virtual void onReadable() = 0;
    virtual void onWritable() = 0;
    virtual void onTimer() {}
};

class Reactor {
public:
    virtual ~Reactor() = default;
    virtual doof::Result<void, std::string> start() = 0;
    virtual void stop() = 0;
    virtual bool addHandler(
        const std::shared_ptr<ReactorHandler>& handler,
        bool wantsRead,
        bool wantsWrite
    ) = 0;
    virtual void updateHandler(int fd, bool wantsRead, bool wantsWrite) = 0;
    virtual void removeHandler(int fd) = 0;
    virtual bool post(std::function<void()> task) = 0;
};

#if defined(__APPLE__)

class KqueueReactor final : public Reactor {
public:
    KqueueReactor() {
        kqueueFd_ = ::kqueue();
        int wakeFds[2] = {-1, -1};
        if (kqueueFd_ >= 0 && ::pipe(wakeFds) == 0) {
            wakeReadFd_ = wakeFds[0];
            wakeWriteFd_ = wakeFds[1];
            (void)setNonBlocking(wakeReadFd_);
            (void)setNonBlocking(wakeWriteFd_);
        }
    }

    ~KqueueReactor() override {
        stop();
        closeSocket(wakeReadFd_);
        closeSocket(wakeWriteFd_);
        if (kqueueFd_ >= 0) {
            ::close(kqueueFd_);
        }
    }

    doof::Result<void, std::string> start() override {
        if (kqueueFd_ < 0 || wakeReadFd_ < 0 || wakeWriteFd_ < 0) {
            return doof::Result<void, std::string>::failure("reactor|failed to initialize kqueue wakeup resources");
        }
        struct kevent wakeEvent;
        EV_SET(&wakeEvent, wakeReadFd_, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (::kevent(kqueueFd_, &wakeEvent, 1, nullptr, 0, nullptr) != 0) {
            return doof::Result<void, std::string>::failure("reactor|" + errnoMessage("failed to register reactor wakeup"));
        }
        running_ = true;
        thread_ = std::thread([this] {
            run();
        });
        return doof::Result<void, std::string>::success();
    }

    void stop() override {
        bool shouldJoin = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                shouldJoin = thread_.joinable();
            } else {
                stopping_ = true;
                shouldJoin = thread_.joinable();
            }
        }
        wake();
        if (shouldJoin) {
            thread_.join();
        }
        handlers_.clear();
    }

    bool addHandler(
        const std::shared_ptr<ReactorHandler>& handler,
        bool wantsRead,
        bool wantsWrite
    ) override {
        if (!handler) {
            return false;
        }
        const int fd = handler->fd();
        if (!applyInterest(fd, wantsRead, wantsWrite)) {
            return false;
        }
        handlers_[fd] = handler;
        return true;
    }

    void updateHandler(int fd, bool wantsRead, bool wantsWrite) override {
        if (handlers_.find(fd) == handlers_.end()) {
            return;
        }
        (void)applyInterest(fd, wantsRead, wantsWrite);
    }

    void removeHandler(int fd) override {
        if (fd < 0) {
            return;
        }
        struct kevent changes[2];
        EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        (void)::kevent(kqueueFd_, changes, 2, nullptr, 0, nullptr);
        handlers_.erase(fd);
    }

    bool post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return false;
            }
            tasks_.push_back(std::move(task));
        }
        wake();
        return true;
    }

private:
    bool applyInterest(int fd, bool wantsRead, bool wantsWrite) {
        struct kevent changes[2];
        EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | (wantsRead ? EV_ENABLE : EV_DISABLE), 0, 0, nullptr);
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | (wantsWrite ? EV_ENABLE : EV_DISABLE), 0, 0, nullptr);
        return ::kevent(kqueueFd_, changes, 2, nullptr, 0, nullptr) == 0;
    }

    void wake() {
        if (wakeWriteFd_ < 0) {
            return;
        }
        const uint8_t byte = 1;
        (void)::write(wakeWriteFd_, &byte, sizeof(byte));
    }

    void drainWakeup() {
        uint8_t buffer[64];
        while (::read(wakeReadFd_, buffer, sizeof(buffer)) > 0) {
        }
    }

    void runPendingTasks() {
        std::deque<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(tasks_);
        }
        for (auto& task : pending) {
            task();
        }
    }

    bool isStopping() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopping_;
    }

    void run() {
        std::vector<struct kevent> events(64);
        while (true) {
            timespec timeout {};
            timeout.tv_nsec = 100 * 1000 * 1000;
            const int count = ::kevent(kqueueFd_, nullptr, 0, events.data(), static_cast<int>(events.size()), &timeout);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }

            for (int index = 0; index < count; ++index) {
                const auto& event = events[static_cast<size_t>(index)];
                const int fd = static_cast<int>(event.ident);
                if (fd == wakeReadFd_) {
                    drainWakeup();
                    runPendingTasks();
                    continue;
                }

                const auto found = handlers_.find(fd);
                if (found == handlers_.end()) {
                    continue;
                }
                auto handler = found->second;
                if (event.filter == EVFILT_READ) {
                    handler->onReadable();
                }
                if (handlers_.find(fd) == handlers_.end()) {
                    continue;
                }
                if (event.filter == EVFILT_WRITE) {
                    handler->onWritable();
                }
            }

            std::vector<std::shared_ptr<ReactorHandler>> handlers;
            handlers.reserve(handlers_.size());
            for (const auto& entry : handlers_) {
                handlers.push_back(entry.second);
            }
            for (const auto& handler : handlers) {
                if (handlers_.find(handler->fd()) != handlers_.end()) {
                    handler->onTimer();
                }
            }

            if (isStopping()) {
                return;
            }
        }
    }

    int kqueueFd_ = -1;
    int wakeReadFd_ = -1;
    int wakeWriteFd_ = -1;
    bool running_ = false;
    bool stopping_ = false;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    std::unordered_map<int, std::shared_ptr<ReactorHandler>> handlers_;
};

#endif

inline doof::Result<std::shared_ptr<Reactor>, std::string> createPlatformReactor() {
#if defined(__APPLE__)
    return doof::Result<std::shared_ptr<Reactor>, std::string>::success(std::make_shared<KqueueReactor>());
#else
    return doof::Result<std::shared_ptr<Reactor>, std::string>::failure(
        "platform|std/http-server reactor is currently implemented only for macOS"
    );
#endif
}

}  // namespace detail

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

class NativeConnection : public detail::ReactorHandler, public std::enable_shared_from_this<NativeConnection> {
public:
    NativeConnection(
        int fd,
        int64_t maxBodyBytes,
        int32_t idleTimeoutMillis,
        int32_t maxRequestsPerConnection,
        std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest,
        std::shared_ptr<detail::Reactor> reactor
    )
        : fd_(fd),
          maxBodyBytes_(maxBodyBytes),
          idleTimeoutMillis_(idleTimeoutMillis),
          maxRequestsPerConnection_(maxRequestsPerConnection),
          onRequest_(std::move(onRequest)),
          reactor_(std::move(reactor)),
          lastActivityAt_(std::chrono::steady_clock::now()) {}

    ~NativeConnection() {
        closeFromServer();
    }

    int fd() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return fd_;
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
            if (closed_) {
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
            int fd = -1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_) {
                    return;
                }
                fd = fd_;
            }

            const ssize_t readCount = ::recv(fd, buffer.data(), buffer.size(), 0);
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
            int fd = -1;
            const uint8_t* data = nullptr;
            size_t remaining = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closed_) {
                    return;
                }
                fd = fd_;
                if (writeOffset_ >= writeBuffer_.size()) {
                    break;
                }
                data = writeBuffer_.data() + writeOffset_;
                remaining = writeBuffer_.size() - writeOffset_;
            }

            const ssize_t written = ::send(fd, data, remaining, 0);
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || idleTimeoutMillis_ == 0 || awaitingResponse_ || !writeBuffer_.empty()) {
                return;
            }
            const auto idleFor = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lastActivityAt_
            );
            shouldClose = idleFor.count() >= idleTimeoutMillis_;
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
            currentFd = fd_;
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
        int oldFd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            oldFd = fd_;
            fd_ = -1;
        }
        detail::closeSocket(oldFd);
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
    int fd_;
    bool closed_ = false;
    bool awaitingResponse_ = false;
    bool closeAfterWrite_ = false;
    int64_t maxBodyBytes_;
    int32_t idleTimeoutMillis_;
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

class NativeHttpServer : public detail::ReactorHandler, public std::enable_shared_from_this<NativeHttpServer> {
public:
    static doof::Result<std::shared_ptr<NativeHttpServer>, std::string> listen(
        const std::string& host,
        int32_t port,
        int64_t maxBodyBytes,
        int32_t idleTimeoutMillis,
        int32_t maxRequestsPerConnection,
        std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest
    ) {
        detail::ignoreSigpipe();
        if (host.empty()) {
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure("bind|host cannot be empty");
        }
        if (port < 0 || port > 65535) {
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure("bind|port must be between 0 and 65535");
        }
        if (maxBodyBytes < 0) {
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure("config|maxBodyBytes must not be negative");
        }
        if (idleTimeoutMillis < 0) {
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure("config|idleTimeoutMillis must not be negative");
        }
        if (maxRequestsPerConnection < 0) {
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure("config|maxRequestsPerConnection must not be negative");
        }

        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo* addresses = nullptr;
        const std::string portText = std::to_string(port);
        const int lookup = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &addresses);
        if (lookup != 0) {
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure(
                "bind|failed to resolve host: " + std::string(gai_strerror(lookup))
            );
        }

        int listenFd = -1;
        std::string lastBindError;
        for (addrinfo* current = addresses; current != nullptr; current = current->ai_next) {
            listenFd = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (listenFd < 0) {
                lastBindError = detail::errnoMessage("failed to create listener socket");
                continue;
            }

            int reuse = 1;
            ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (::bind(listenFd, current->ai_addr, current->ai_addrlen) == 0 &&
                ::listen(listenFd, SOMAXCONN) == 0) {
                break;
            }

            lastBindError = detail::errnoMessage("failed to bind listener");
            detail::closeSocket(listenFd);
            listenFd = -1;
        }
        ::freeaddrinfo(addresses);

        if (listenFd < 0) {
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure(
                "bind|" + (lastBindError.empty() ? "failed to bind listener" : lastBindError)
            );
        }

        sockaddr_in bound {};
        socklen_t boundLength = sizeof(bound);
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0) {
            const std::string error = detail::errnoMessage("failed to inspect listener");
            detail::closeSocket(listenFd);
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure("listen|" + error);
        }

        auto reactorResult = detail::createPlatformReactor();
        if (reactorResult.isFailure()) {
            detail::closeSocket(listenFd);
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure(reactorResult.error());
        }

        auto server = std::shared_ptr<NativeHttpServer>(
            new NativeHttpServer(
                host,
                ntohs(bound.sin_port),
                maxBodyBytes,
                idleTimeoutMillis,
                maxRequestsPerConnection,
                std::move(onRequest),
                listenFd,
                reactorResult.value()
            )
        );
        auto started = server->start();
        if (started.isFailure()) {
            detail::closeSocket(listenFd);
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure(started.error());
        }
        retain(server);
        return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::success(std::move(server));
    }

    ~NativeHttpServer() {
        (void)close();
    }

    std::string host() const {
        return host_;
    }

    int32_t port() const {
        return port_;
    }

    int fd() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return listenFd_;
    }

    void onReadable() override {
        acceptReadyConnections();
    }

    void onWritable() override {}

    doof::Result<void, std::string> close() {
        std::vector<std::shared_ptr<NativeConnection>> connections;
        int listenFd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return doof::Result<void, std::string>::failure("closed|server has already been closed");
            }
            closed_ = true;
            listenFd = listenFd_;
            listenFd_ = -1;
            for (const auto& weak : activeConnections_) {
                if (auto connection = weak.lock()) {
                    connections.push_back(std::move(connection));
                }
            }
        }

        reactor_->stop();
        detail::closeSocket(listenFd);
        for (const auto& connection : connections) {
            connection->closeFromServer();
        }

        release(this);
        return doof::Result<void, std::string>::success();
    }

private:
    NativeHttpServer(
        std::string host,
        int32_t port,
        int64_t maxBodyBytes,
        int32_t idleTimeoutMillis,
        int32_t maxRequestsPerConnection,
        std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest,
        int listenFd,
        std::shared_ptr<detail::Reactor> reactor
    )
        : host_(std::move(host)),
          port_(port),
          maxBodyBytes_(maxBodyBytes),
          idleTimeoutMillis_(idleTimeoutMillis),
          maxRequestsPerConnection_(maxRequestsPerConnection),
          onRequest_(std::move(onRequest)),
          listenFd_(listenFd),
          reactor_(std::move(reactor)) {}

    doof::Result<void, std::string> start() {
        auto nonBlocking = detail::setNonBlocking(listenFd_);
        if (nonBlocking.isFailure()) {
            return nonBlocking;
        }
        if (!reactor_->addHandler(shared_from_this(), true, false)) {
            return doof::Result<void, std::string>::failure("reactor|failed to register listener");
        }
        return reactor_->start();
    }

    void acceptReadyConnections() {
        while (true) {
            sockaddr_in peer {};
            socklen_t peerLength = sizeof(peer);
            const int clientFd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (clientFd >= 0) {
                auto nonBlocking = detail::setNonBlocking(clientFd);
                if (nonBlocking.isFailure()) {
                    detail::closeSocket(clientFd);
                    continue;
                }

                std::shared_ptr<NativeConnection> connection;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (closed_) {
                        detail::closeSocket(clientFd);
                        return;
                    }
                    pruneConnectionsLocked();
                    connection = std::make_shared<NativeConnection>(
                        clientFd,
                        maxBodyBytes_,
                        idleTimeoutMillis_,
                        maxRequestsPerConnection_,
                        onRequest_,
                        reactor_
                    );
                    activeConnections_.push_back(connection);
                }
                if (!reactor_->addHandler(connection, true, false)) {
                    connection->closeFromServer();
                }
                continue;
            }

            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            return;
        }
    }

    static void retain(const std::shared_ptr<NativeHttpServer>& server) {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().push_back(server);
    }

    static void release(const NativeHttpServer* server) {
        std::lock_guard<std::mutex> lock(registryMutex());
        auto& items = registry();
        items.erase(
            std::remove_if(items.begin(), items.end(), [server](const auto& item) {
                return item.get() == server;
            }),
            items.end()
        );
    }

    static std::vector<std::shared_ptr<NativeHttpServer>>& registry() {
        static std::vector<std::shared_ptr<NativeHttpServer>> servers;
        return servers;
    }

    static std::mutex& registryMutex() {
        static std::mutex mutex;
        return mutex;
    }

    void pruneConnectionsLocked() {
        activeConnections_.erase(
            std::remove_if(activeConnections_.begin(), activeConnections_.end(), [](const auto& connection) {
                return connection.expired();
            }),
            activeConnections_.end()
        );
    }

    std::string host_;
    int32_t port_;
    int64_t maxBodyBytes_;
    int32_t idleTimeoutMillis_;
    int32_t maxRequestsPerConnection_;
    std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest_;

    mutable std::mutex mutex_;
    bool closed_ = false;
    int listenFd_;
    std::shared_ptr<detail::Reactor> reactor_;
    std::vector<std::weak_ptr<NativeConnection>> activeConnections_;
};

}  // namespace doof_http_server
