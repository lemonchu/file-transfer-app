#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace ft {

using Clock = std::chrono::steady_clock;
#ifdef _WIN32
using Handle = SOCKET;
using SocketLength = int;
using BufferLength = int;
constexpr Handle invalid_socket = INVALID_SOCKET;
#else
using Handle = int;
using SocketLength = socklen_t;
using BufferLength = std::size_t;
constexpr Handle invalid_socket = -1;
#endif

void check_cancelled();

class Network {
public:
    Network();
    ~Network();
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
};

class Socket {
public:
    explicit Socket(Handle handle = invalid_socket) noexcept : handle_(handle) {}
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Handle get() const noexcept { return handle_; }
    void send_all(const void* data, std::size_t size, int timeout) const;
    void receive_all(void* data, std::size_t size, int timeout) const;
    void finish_sending() const;
    void expect_end(int timeout) const;
private:
    Handle handle_;
};

Socket connect_to(const std::string& host, const std::string& port, int timeout);
Socket listen_on(const std::string& host, const std::string& port);
Socket accept_one(const Socket& listener, int timeout);
std::vector<std::string> local_addresses();

} // namespace ft
