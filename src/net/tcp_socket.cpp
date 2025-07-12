#include "net/tcp_socket.hpp"
#include "io_awaitable.hpp"
#include "detail/os_backend.hpp"
#include <cstring>
#include <errno.h>

#ifdef SWIFTNET_PLATFORM_WINDOWS
    #include <poll.h>
#else
    #include <poll.h>
#endif

using namespace swiftnet::net;

tcp_socket::tcp_socket(int fd) : fd_(fd)
{
    if (fd_ != -1)
        set_nonblock();
}

tcp_socket::tcp_socket(tcp_socket &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

tcp_socket &tcp_socket::operator=(tcp_socket &&o) noexcept
{
    if (this != &o)
    {
        close();
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

tcp_socket::~tcp_socket() { close(); }

void tcp_socket::set_nonblock()
{
    detail::platform::make_socket_nonblocking(fd_);
}

void tcp_socket::close()
{
    if (fd_ != -1)
    {
        detail::platform::close_socket(fd_);
        fd_ = -1;
    }
}

swiftnet::vthread_base<int> tcp_socket::async_read(void *buf, std::size_t len)
{
    std::size_t read_total = 0;
    while (read_total < len)
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        ssize_t r = recv(fd_, (char *)buf + read_total, len - read_total, 0);
#else
        ssize_t r = ::read(fd_, (char *)buf + read_total, len - read_total);
#endif
        if (r > 0)
        {
            read_total += r;
            continue;
        }
        if (r == 0)
            co_return static_cast<int>(read_total);
            
#ifdef SWIFTNET_PLATFORM_WINDOWS
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
            co_await io_awaitable(fd_, POLLIN);
        else
            co_return -1;
    }
    co_return static_cast<int>(read_total);
}

swiftnet::vthread_base<int> tcp_socket::async_write(const void *buf, std::size_t len)
{
    std::size_t written = 0;
    while (written < len)
    {
#ifdef SWIFTNET_PLATFORM_WINDOWS
        ssize_t w = send(fd_, (const char *)buf + written, len - written, 0);
#else
        ssize_t w = ::write(fd_, (const char *)buf + written, len - written);
#endif
        if (w > 0)
        {
            written += w;
            continue;
        }
        
#ifdef SWIFTNET_PLATFORM_WINDOWS
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
            co_await io_awaitable(fd_, POLLOUT);
        else
            co_return -1;
    }
    co_return static_cast<int>(written);
}
