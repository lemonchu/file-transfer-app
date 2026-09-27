#include "net.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace ft {
namespace {
volatile std::sig_atomic_t cancelled = 0;
void on_signal(int) { cancelled = 1; }

int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::runtime_error network_error(const std::string& operation, int code = last_error()) {
    return std::runtime_error(operation + ": " +
        std::system_category().message(code) + " (" + std::to_string(code) + ")");
}

bool retryable(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINTR || code == WSAEINPROGRESS;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == EINTR || code == EINPROGRESS;
#endif
}

void close_socket(Handle handle) noexcept {
    if (handle == invalid_socket) return;
#ifdef _WIN32
    closesocket(handle);
#else
    close(handle);
#endif
}

void configure(Handle handle) {
#ifdef _WIN32
    u_long enabled = 1;
    if (ioctlsocket(handle, FIONBIO, &enabled) != 0) throw network_error("Nonblocking socket");
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0)
        throw network_error("Nonblocking socket");
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
        throw network_error("SO_NOSIGPIPE");
#endif
#endif
}

// Nonblocking I/O plus a deadline handles short writes and stalled peers on all platforms.
void wait_ready(Handle handle, bool writing, Clock::time_point deadline) {
    for (;;) {
        check_cancelled();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) throw std::runtime_error("Network timeout");
        const int milliseconds = static_cast<int>(std::min<std::int64_t>(remaining.count(), 200));
#ifdef _WIN32
        fd_set ready, errors;
        FD_ZERO(&ready);
        FD_ZERO(&errors);
        FD_SET(handle, &ready);
        FD_SET(handle, &errors);
        timeval tv{0, milliseconds * 1000};
        const int result = select(0, writing ? nullptr : &ready,
                                  writing ? &ready : nullptr, &errors, &tv);
#else
        pollfd item{handle, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int result = poll(&item, 1, milliseconds);
#endif
        if (result > 0) return;
        if (result < 0 && !retryable(last_error())) throw network_error("Wait for socket");
    }
}

using Addresses = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;
Addresses resolve(const std::string& host, const std::string& port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
    addrinfo* result = nullptr;
    const int code = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (code != 0) {
#ifdef _WIN32
        const char* message = gai_strerrorA(code);
#else
        const char* message = gai_strerror(code);
#endif
        throw std::runtime_error("Cannot resolve '" + host + "': " + message);
    }
    return Addresses(result, freeaddrinfo);
}

} // namespace

void check_cancelled() {
    if (cancelled) throw std::runtime_error("Transfer cancelled");
}

Network::Network() {
#ifdef _WIN32
    WSADATA data{};
    const int code = WSAStartup(MAKEWORD(2, 2), &data);
    if (code != 0) throw network_error("WSAStartup", code);
#endif
    cancelled = 0;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
}

Network::~Network() {
#ifdef _WIN32
    WSACleanup();
#endif
}

Socket::~Socket() { close_socket(handle_); }
Socket::Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, invalid_socket)) {}
Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close_socket(handle_);
        handle_ = std::exchange(other.handle_, invalid_socket);
    }
    return *this;
}

void Socket::send_all(const void* data, std::size_t size, int timeout) const {
    const auto* bytes = static_cast<const char*>(data);
    auto deadline = Clock::now() + std::chrono::seconds(timeout);
    while (size > 0) {
        wait_ready(handle_, true, deadline);
        const auto count = static_cast<BufferLength>(std::min<std::size_t>(size, 64 * 1024));
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        const auto sent = send(handle_, bytes, count, flags);
        if (sent < 0) {
            if (retryable(last_error())) continue;
            throw network_error("Send");
        }
        if (sent == 0) throw std::runtime_error("Peer stopped accepting data");
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
        deadline = Clock::now() + std::chrono::seconds(timeout);
    }
}

void Socket::receive_all(void* data, std::size_t size, int timeout) const {
    auto* bytes = static_cast<char*>(data);
    auto deadline = Clock::now() + std::chrono::seconds(timeout);
    while (size > 0) {
        wait_ready(handle_, false, deadline);
        const auto count = static_cast<BufferLength>(std::min<std::size_t>(size, 64 * 1024));
        const auto received = recv(handle_, bytes, count, 0);
        if (received < 0) {
            if (retryable(last_error())) continue;
            throw network_error("Receive");
        }
        if (received == 0) throw std::runtime_error("Peer closed connection before transfer completed");
        bytes += received;
        size -= static_cast<std::size_t>(received);
        deadline = Clock::now() + std::chrono::seconds(timeout);
    }
}

void Socket::finish_sending() const {
#ifdef _WIN32
    constexpr int direction = SD_SEND;
#else
    constexpr int direction = SHUT_WR;
#endif
    if (shutdown(handle_, direction) != 0) throw network_error("Finish sending");
}

void Socket::expect_end(int timeout) const {
    const auto deadline = Clock::now() + std::chrono::seconds(timeout);
    for (;;) {
        wait_ready(handle_, false, deadline);
        char extra;
        const auto result = recv(handle_, &extra, 1, 0);
        if (result == 0) return;
        if (result > 0) throw std::runtime_error("Unexpected data after checksum");
        if (!retryable(last_error())) throw network_error("End of transfer");
    }
}

Socket connect_to(const std::string& host, const std::string& port, int timeout) {
    const auto addresses = resolve(host, port, false);
    std::string failure = "No usable address";
    // One connection budget for all resolved addresses (DNS lookup is OS-managed).
    const auto deadline = Clock::now() + std::chrono::seconds(timeout);
    for (auto* item = addresses.get(); item; item = item->ai_next) {
        check_cancelled();
        try {
            Socket socket(::socket(item->ai_family, item->ai_socktype, item->ai_protocol));
            if (socket.get() == invalid_socket) throw network_error("Create socket");
            configure(socket.get());
            if (connect(socket.get(), item->ai_addr, static_cast<SocketLength>(item->ai_addrlen)) != 0) {
                if (!retryable(last_error())) throw network_error("Connect");
                wait_ready(socket.get(), true, deadline);
                int error = 0;
                SocketLength length = sizeof(error);
                if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0)
                    throw network_error("Connect status");
                if (error != 0) throw network_error("Connect", error);
            }
            return socket;
        } catch (const std::exception& error) { failure = error.what(); }
    }
    throw std::runtime_error("Cannot connect to " + host + ":" + port + ": " + failure);
}

Socket listen_on(const std::string& host, const std::string& port) {
    const auto addresses = resolve(host, port, true);
    std::string failure = "No usable address";
    for (auto* item = addresses.get(); item; item = item->ai_next) {
        try {
            Socket socket(::socket(item->ai_family, item->ai_socktype, item->ai_protocol));
            if (socket.get() == invalid_socket) throw network_error("Create socket");
            configure(socket.get());
            int enabled = 1;
#ifdef _WIN32
            constexpr int option = SO_EXCLUSIVEADDRUSE;
#else
            constexpr int option = SO_REUSEADDR;
#endif
            if (setsockopt(socket.get(), SOL_SOCKET, option, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
                throw network_error("Set listener options");
            if (bind(socket.get(), item->ai_addr, static_cast<SocketLength>(item->ai_addrlen)) != 0)
                throw network_error("Bind");
            if (listen(socket.get(), 1) != 0) throw network_error("Listen");
            return socket;
        } catch (const std::exception& error) { failure = error.what(); }
    }
    throw std::runtime_error("Cannot listen on " + host + ":" + port + ": " + failure);
}

Socket accept_one(const Socket& listener, int timeout) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeout);
    for (;;) {
        wait_ready(listener.get(), false, deadline);
        Socket socket(accept(listener.get(), nullptr, nullptr));
        if (socket.get() != invalid_socket) {
            configure(socket.get());
            return socket;
        }
        if (!retryable(last_error())) throw network_error("Accept");
    }
}

std::vector<std::string> local_addresses() {
    std::vector<std::string> result;
    auto add = [&](const sockaddr* address, SocketLength length) {
        if (!address || (address->sa_family != AF_INET && address->sa_family != AF_INET6)) return;
        char host[NI_MAXHOST]{};
        if (getnameinfo(address, length, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) == 0)
            result.emplace_back(host);
    };
#ifdef _WIN32
    ULONG length = 16384;
    std::vector<unsigned char> buffer(length);
    ULONG code = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && code == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(length);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        code = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, adapters, &length);
        if (code == NO_ERROR) {
            for (auto* adapter = adapters; adapter; adapter = adapter->Next)
                for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next)
                    add(entry->Address.lpSockaddr, entry->Address.iSockaddrLength);
        }
    }
    if (code != NO_ERROR && code != ERROR_NO_DATA) throw network_error("List local addresses", static_cast<int>(code));
#else
    ifaddrs* raw = nullptr;
    if (getifaddrs(&raw) != 0) throw network_error("List local addresses");
    std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> addresses(raw, freeifaddrs);
    for (auto* entry = raw; entry; entry = entry->ifa_next) {
        if (entry->ifa_addr)
            add(entry->ifa_addr, entry->ifa_addr->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
    }
#endif
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace ft
