#include "net/acceptor.hpp"
#include "io_context.hpp"
#include "io_awaitable.hpp"
#include "vthread_scheduler.hpp"
#include "detail/os_backend.hpp"
#include <cstring>
#include <iostream>

#ifdef SWIFTNET_PLATFORM_WINDOWS
    #include <poll.h>
#else
    #include <errno.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

using namespace swiftnet::net;

acceptor::acceptor(uint16_t port, int backlog)
{
    // Initialize networking on Windows
    detail::platform::init_networking();
    
#ifdef SWIFTNET_PLATFORM_WINDOWS
    listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
#endif
    
    if (listen_fd_ < 0)
        throw std::runtime_error("socket creation failed");

    std::cout << "[DEBUG] Created listen socket fd=" << listen_fd_ << " for port " << port << std::endl;

    // Set non-blocking
    set_nonblock(listen_fd_);
    
    // DEBUG: Check socket flags after setting non-blocking
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    std::cout << "[DEBUG] Socket flags after non-blocking: " << flags << " (O_NONBLOCK=" << O_NONBLOCK << ")" << std::endl;

    int opt = 1;
#ifdef SWIFTNET_PLATFORM_WINDOWS
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    #ifdef SO_REUSEPORT
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    #endif
#endif

    std::cout << "[DEBUG] Set socket options (REUSEADDR, REUSEPORT)" << std::endl;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0) {
        detail::platform::close_socket(listen_fd_);
        throw std::runtime_error("bind failed: " + detail::platform::get_error_string(detail::platform::get_last_socket_error()));
    }
    
    std::cout << "[DEBUG] Bound socket to port " << port << std::endl;
    
    if (listen(listen_fd_, backlog) < 0) {
        detail::platform::close_socket(listen_fd_);
        throw std::runtime_error("listen failed: " + detail::platform::get_error_string(detail::platform::get_last_socket_error()));
    }

    std::cout << "[DEBUG] Socket listening on port " << port << " fd=" << listen_fd_ << " backlog=" << backlog << std::endl;
    
    // DEBUG: Test if the socket is immediately ready for accept (should return EAGAIN)
    sockaddr_in test_addr{};
    socklen_t test_len = sizeof(test_addr);
    int test_fd = accept(listen_fd_, (sockaddr*)&test_addr, &test_len);
    if (test_fd < 0) {
        std::cout << "[DEBUG] Initial accept() test: " << strerror(errno) << " (expected: Resource temporarily unavailable)" << std::endl;
    } else {
        std::cout << "[DEBUG] WARNING: accept() immediately succeeded with fd=" << test_fd << " (unexpected!)" << std::endl;
        close(test_fd);
    }

    // io_awaitable will poll readability on listen_fd_ in async_accept()
}

acceptor::~acceptor() { 
    detail::platform::close_socket(listen_fd_);
    detail::platform::cleanup_networking();
}

void acceptor::set_nonblock(int fd)
{
    detail::platform::make_socket_nonblocking(fd);
}

swiftnet::vthread acceptor::async_accept(std::function<void(tcp_socket)> cb)
{
    std::cout << "[DEBUG] async_accept starting with listen_fd_=" << listen_fd_ << std::endl;
    
    try {
        while (true)
        {
            std::cout << "[DEBUG] async_accept: top of accept loop" << std::endl;
            
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            
            std::cout << "[DEBUG] async_accept: calling platform_accept on fd=" << listen_fd_ << std::endl;
            int client_fd = detail::platform::platform_accept(listen_fd_, (sockaddr *)&addr, &len);
            std::cout << "[DEBUG] async_accept: platform_accept returned client_fd=" << client_fd << " errno=" << errno << std::endl;
            
            if (client_fd >= 0)
            {
                std::cout << "[DEBUG] Accepted connection: client_fd=" << client_fd << std::endl;
                // Set client socket to non-blocking
                set_nonblock(client_fd);
                std::cout << "[DEBUG] Calling callback with client socket" << std::endl;
                cb(tcp_socket(client_fd));
                std::cout << "[DEBUG] Callback completed, continuing loop" << std::endl;
                continue;
            }

    #ifdef SWIFTNET_PLATFORM_WINDOWS
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
    #else
            std::cout << "[DEBUG] Accept failed, errno=" << errno << " EAGAIN=" << EAGAIN << " EWOULDBLOCK=" << EWOULDBLOCK << std::endl;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
    #endif
            {
                std::cout << "[DEBUG] No connection ready, waiting for I/O on listen_fd_=" << listen_fd_ << std::endl;
                
                try {
                    std::cout << "[DEBUG] About to co_await io_awaitable..." << std::endl;
                    int io_result = co_await io_awaitable(listen_fd_, POLLIN, false);
                    std::cout << "[DEBUG] I/O awaitable returned with result=" << io_result << std::endl;
                    
                    if (io_result == -2) {
                        std::cout << "[DEBUG] I/O timeout (expected), continuing accept loop..." << std::endl;
                        continue; // Timeout is expected, just try again
                    } else if (io_result < 0) {
                        std::cout << "[DEBUG] I/O error (" << io_result << "), continuing accept loop..." << std::endl;
                        continue; // Other errors, try again  
                    } else {
                        std::cout << "[DEBUG] I/O ready, continuing to accept..." << std::endl;
                        continue; // I/O ready, try accept again
                    }
                } catch (const std::exception& e) {
                    std::cout << "[DEBUG] EXCEPTION in io_awaitable: " << e.what() << std::endl;
                    std::cout << "[DEBUG] async_accept: exiting due to io_awaitable exception" << std::endl;
                    co_return; // Exit the coroutine on exception
                }
                
                continue;
            }
            else
            {
                std::cerr << "accept error: " << detail::platform::get_error_string(detail::platform::get_last_socket_error()) << "\n";
                std::cout << "[DEBUG] async_accept: exiting due to accept error, errno=" << errno << std::endl;
                co_return;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[DEBUG] OUTER EXCEPTION in async_accept: " << e.what() << std::endl;
        co_return;
    } catch (...) {
        std::cout << "[DEBUG] UNKNOWN EXCEPTION in async_accept" << std::endl;
        co_return;
    }
    
    std::cout << "[DEBUG] async_accept: should never reach here!" << std::endl;
}
