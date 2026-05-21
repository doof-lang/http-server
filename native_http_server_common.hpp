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
#include <poll.h>
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

inline void closeSocket(int fd) {
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
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

}  // namespace detail
}  // namespace doof_http_server
