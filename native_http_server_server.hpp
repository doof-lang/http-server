#pragma once

#include "native_http_server_connection.hpp"

namespace doof_http_server {

class NativeHttpServer : public detail::ReactorHandler, public std::enable_shared_from_this<NativeHttpServer> {
public:
    static doof::Result<std::shared_ptr<NativeHttpServer>, std::string> listen(
        const std::string& host,
        int32_t port,
        int64_t maxBodyBytes,
        int32_t idleTimeoutMillis,
        int32_t responseTimeoutMillis,
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
        if (responseTimeoutMillis < 0) {
            return doof::Result<std::shared_ptr<NativeHttpServer>, std::string>::failure("config|responseTimeoutMillis must not be negative");
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
                responseTimeoutMillis,
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
        int32_t responseTimeoutMillis,
        int32_t maxRequestsPerConnection,
        std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest,
        int listenFd,
        std::shared_ptr<detail::Reactor> reactor
    )
        : host_(std::move(host)),
          port_(port),
          maxBodyBytes_(maxBodyBytes),
          idleTimeoutMillis_(idleTimeoutMillis),
          responseTimeoutMillis_(responseTimeoutMillis),
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
                        std::make_shared<detail::PlainSocketTransport>(clientFd),
                        maxBodyBytes_,
                        idleTimeoutMillis_,
                        responseTimeoutMillis_,
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
    int32_t responseTimeoutMillis_;
    int32_t maxRequestsPerConnection_;
    std::function<int32_t(std::shared_ptr<NativeExchange>)> onRequest_;

    mutable std::mutex mutex_;
    bool closed_ = false;
    int listenFd_;
    std::shared_ptr<detail::Reactor> reactor_;
    std::vector<std::weak_ptr<NativeConnection>> activeConnections_;
};

}  // namespace doof_http_server
