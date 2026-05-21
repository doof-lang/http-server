#pragma once

#include "native_http_server_common.hpp"

namespace doof_http_server {
namespace detail {

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

class PollReactor final : public Reactor {
public:
    PollReactor() {
        int wakeFds[2] = {-1, -1};
        if (::pipe(wakeFds) == 0) {
            wakeReadFd_ = wakeFds[0];
            wakeWriteFd_ = wakeFds[1];
            (void)setNonBlocking(wakeReadFd_);
            (void)setNonBlocking(wakeWriteFd_);
        }
    }

    ~PollReactor() override {
        stop();
        closeSocket(wakeReadFd_);
        closeSocket(wakeWriteFd_);
    }

    doof::Result<void, std::string> start() override {
        if (wakeReadFd_ < 0 || wakeWriteFd_ < 0) {
            return doof::Result<void, std::string>::failure("reactor|failed to initialize poll wakeup resources");
        }
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
        interests_.clear();
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
        handlers_[fd] = handler;
        interests_[fd] = Interest { wantsRead, wantsWrite };
        wake();
        return true;
    }

    void updateHandler(int fd, bool wantsRead, bool wantsWrite) override {
        if (handlers_.find(fd) == handlers_.end()) {
            return;
        }
        interests_[fd] = Interest { wantsRead, wantsWrite };
        wake();
    }

    void removeHandler(int fd) override {
        if (fd < 0) {
            return;
        }
        handlers_.erase(fd);
        interests_.erase(fd);
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
    struct Interest {
        bool wantsRead = false;
        bool wantsWrite = false;
    };

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

    void runTimers() {
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
    }

    void run() {
        while (true) {
            std::vector<pollfd> fds;
            fds.push_back(pollfd { wakeReadFd_, POLLIN, 0 });
            for (const auto& entry : handlers_) {
                const auto interest = interests_[entry.first];
                short events = 0;
                if (interest.wantsRead) {
                    events |= POLLIN;
                }
                if (interest.wantsWrite) {
                    events |= POLLOUT;
                }
                fds.push_back(pollfd { entry.first, events, 0 });
            }

            const int count = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }

            if (count > 0) {
                for (const auto& event : fds) {
                    if (event.revents == 0) {
                        continue;
                    }
                    if (event.fd == wakeReadFd_) {
                        drainWakeup();
                        runPendingTasks();
                        continue;
                    }

                    const auto found = handlers_.find(event.fd);
                    if (found == handlers_.end()) {
                        continue;
                    }
                    auto handler = found->second;
                    if ((event.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                        handler->onReadable();
                    }
                    if (handlers_.find(event.fd) == handlers_.end()) {
                        continue;
                    }
                    if ((event.revents & POLLOUT) != 0) {
                        handler->onWritable();
                    }
                }
            }

            runTimers();

            if (isStopping()) {
                return;
            }
        }
    }

    int wakeReadFd_ = -1;
    int wakeWriteFd_ = -1;
    bool stopping_ = false;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    std::unordered_map<int, Interest> interests_;
    std::unordered_map<int, std::shared_ptr<ReactorHandler>> handlers_;
};

inline doof::Result<std::shared_ptr<Reactor>, std::string> createPlatformReactor() {
#if defined(__APPLE__)
    return doof::Result<std::shared_ptr<Reactor>, std::string>::success(std::make_shared<KqueueReactor>());
#else
    return doof::Result<std::shared_ptr<Reactor>, std::string>::success(std::make_shared<PollReactor>());
#endif
}

}  // namespace detail
}  // namespace doof_http_server
