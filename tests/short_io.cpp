// Linked only into Linux test binaries. Exercise the real socket loops while forcing
// short reads/writes and transient errors that normal loopback tests rarely produce.
#include <algorithm>
#include <cerrno>
#include <sys/socket.h>

extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __real_recv(int, void*, size_t, int);

extern "C" ssize_t __wrap_send(int socket, const void* data, size_t size, int flags) {
    static unsigned calls = 0;
    if (++calls % 17 == 0) { errno = EINTR; return -1; }
    if (calls % 19 == 0) { errno = EAGAIN; return -1; }
    return __real_send(socket, data, std::min<size_t>(size, 137), flags);
}

extern "C" ssize_t __wrap_recv(int socket, void* data, size_t size, int flags) {
    static unsigned calls = 0;
    if (++calls % 13 == 0) { errno = EINTR; return -1; }
    if (calls % 23 == 0) { errno = EAGAIN; return -1; }
    return __real_recv(socket, data, std::min<size_t>(size, 113), flags);
}
