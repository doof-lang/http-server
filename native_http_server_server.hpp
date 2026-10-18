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
        bool keepsAlive,
        doof::callback<void(std::shared_ptr<NativeExchange>)> onRequest
    ) {
        detail::ignoreSigpipe();
        if (host.empty()) {
            return doof::Failure<std::string>{"bind|host cannot be empty"};
        }
        if (port < 0 || port > 65535) {
            return doof::Failure<std::string>{"bind|port must be between 0 and 65535"};
        }
        if (maxBodyBytes < 0) {
            return doof::Failure<std::string>{"config|maxBodyBytes must not be negative"};
        }
        if (idleTimeoutMillis < 0) {
            return doof::Failure<std::string>{"config|idleTimeoutMillis must not be negative"};
        }
        if (responseTimeoutMillis < 0) {
            return doof::Failure<std::string>{"config|responseTimeoutMillis must not be negative"};
        }
        if (maxRequestsPerConnection < 0) {
            return doof::Failure<std::string>{"config|maxRequestsPerConnection must not be negative"};
        }

        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        addrinfo* addresses = nullptr;
        const std::string portText = std::to_string(port);
        const int lookup = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &addresses);
        if (lookup != 0) {
            return doof::Failure<std::string>{"bind|failed to resolve host: " + std::string(gai_strerror(lookup))};
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
            return doof::Failure<std::string>{"bind|" + (lastBindError.empty() ? "failed to bind listener" : lastBindError)};
        }

        sockaddr_in bound {};
        socklen_t boundLength = sizeof(bound);
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0) {
            const std::string error = detail::errnoMessage("failed to inspect listener");
            detail::closeSocket(listenFd);
            return doof::Failure<std::string>{"listen|" + error};
        }

        // The first reactor also accepts. Accepted connections are spread
        // across all of them so socket I/O is not limited to one thread.
        std::vector<std::shared_ptr<detail::Reactor>> reactors;
        for (size_t index = 0; index < ioThreadCount(); ++index) {
            auto reactorResult = detail::createPlatformReactor();
            if (doof::is_failure(reactorResult)) {
                detail::closeSocket(listenFd);
                return doof::Failure<std::string>{doof::failure_error(reactorResult)};
            }
            reactors.push_back(doof::success_value(reactorResult));
        }

        auto server = std::shared_ptr<NativeHttpServer>(
            new NativeHttpServer(
                host,
                ntohs(bound.sin_port),
                maxBodyBytes,
                idleTimeoutMillis,
                responseTimeoutMillis,
                maxRequestsPerConnection,
                std::make_shared<detail::RequestDispatcher>(std::move(onRequest)),
                listenFd,
                std::move(reactors)
            )
        );
        auto started = server->start();
        if (doof::is_failure(started)) {
            detail::closeSocket(listenFd);
            return doof::Failure<std::string>{doof::failure_error(started)};
        }
        server->retainMainEventLoop(keepsAlive);
        retain(server);
        return doof::Success<std::shared_ptr<NativeHttpServer>>{std::move(server)};
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

    // Replaces the callbacks that receive new requests. Requests already
    // posted to a handler stay with it.
    doof::Result<void, std::string> setHandlers(
        const std::shared_ptr<std::vector<detail::ExchangeHandler>>& handlers
    ) {
        if (!handlers || handlers->empty()) {
            return doof::Failure<std::string>{"config|handlers must not be empty"};
        }
        dispatcher_->setHandlers(*handlers);
        return doof::Success<void>{};
    }

    // Requests answered with 503 because every handler's mailbox was full.
    int64_t shedCount() const {
        return dispatcher_->shedCount();
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
                return doof::Failure<std::string>{"closed|server has already been closed"};
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

        for (const auto& reactor : reactors_) {
            reactor->stop();
        }
        detail::closeSocket(listenFd);
        for (const auto& connection : connections) {
            connection->closeFromServer();
        }

        releaseMainEventLoop();
        release(this);
        return doof::Success<void>{};
    }

private:
    NativeHttpServer(
        std::string host,
        int32_t port,
        int64_t maxBodyBytes,
        int32_t idleTimeoutMillis,
        int32_t responseTimeoutMillis,
        int32_t maxRequestsPerConnection,
        std::shared_ptr<detail::RequestDispatcher> dispatcher,
        int listenFd,
        std::vector<std::shared_ptr<detail::Reactor>> reactors
    )
        : host_(std::move(host)),
          port_(port),
          maxBodyBytes_(maxBodyBytes),
          idleTimeoutMillis_(idleTimeoutMillis),
          responseTimeoutMillis_(responseTimeoutMillis),
          maxRequestsPerConnection_(maxRequestsPerConnection),
          dispatcher_(std::move(dispatcher)),
          listenFd_(listenFd),
          reactors_(std::move(reactors)) {}

    // One I/O thread per four cores, capped at four. Each request already
    // crosses to the handler's actor, so I/O threads only read, parse and
    // dispatch; a few are enough to keep many workers busy.
    static size_t ioThreadCount() {
        const unsigned cores = std::thread::hardware_concurrency();
        return std::max<size_t>(1, std::min<size_t>(4, cores / 4));
    }

    doof::Result<void, std::string> start() {
        auto nonBlocking = detail::setNonBlocking(listenFd_);
        if (doof::is_failure(nonBlocking)) {
            return nonBlocking;
        }
        if (!reactors_.front()->addHandler(shared_from_this(), true, false)) {
            return doof::Failure<std::string>{"reactor|failed to register listener"};
        }
        for (const auto& reactor : reactors_) {
            auto started = reactor->start();
            if (doof::is_failure(started)) {
                for (const auto& running : reactors_) {
                    running->stop();
                }
                return started;
            }
        }
        return doof::Success<void>{};
    }

    void acceptReadyConnections() {
        while (true) {
            sockaddr_in peer {};
            socklen_t peerLength = sizeof(peer);
            const int clientFd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (clientFd >= 0) {
                auto nonBlocking = detail::setNonBlocking(clientFd);
                if (doof::is_failure(nonBlocking)) {
                    detail::closeSocket(clientFd);
                    continue;
                }

                std::shared_ptr<NativeConnection> connection;
                const auto& reactor = reactors_[nextReactor_];
                nextReactor_ = (nextReactor_ + 1) % reactors_.size();
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
                        dispatcher_,
                        reactor
                    );
                    activeConnections_.push_back(connection);
                }
                if (!addConnectionToReactor(reactor, connection)) {
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

    // Reactor handler tables belong to their reactor thread, so a connection
    // owned by another reactor is registered from inside that reactor.
    static bool addConnectionToReactor(
        const std::shared_ptr<detail::Reactor>& reactor,
        const std::shared_ptr<NativeConnection>& connection
    ) {
        if (reactor->isReactorThread()) {
            return reactor->addHandler(connection, true, false);
        }
        return reactor->post([reactor, connection] {
            if (!reactor->addHandler(connection, true, false)) {
                connection->closeFromServer();
            }
        });
    }

    // An open server keeps the root event loop running, as a long-lived
    // event source, until it is closed.
    void retainMainEventLoop(bool keepsAlive) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!keepsAlive || closed_) {
            return;
        }
        doof::detail::ApplicationDomain::shared().add_keep_alive_source(true);
        keepingAlive_ = true;
    }

    void releaseMainEventLoop() {
        bool keptAlive = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            keptAlive = keepingAlive_;
            keepingAlive_ = false;
        }
        if (keptAlive) {
            doof::detail::ApplicationDomain::shared().remove_keep_alive_source(true);
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
    std::shared_ptr<detail::RequestDispatcher> dispatcher_;

    mutable std::mutex mutex_;
    bool closed_ = false;
    bool keepingAlive_ = false;
    int listenFd_;
    std::vector<std::shared_ptr<detail::Reactor>> reactors_;
    size_t nextReactor_ = 0;
    std::vector<std::weak_ptr<NativeConnection>> activeConnections_;
};

}  // namespace doof_http_server
