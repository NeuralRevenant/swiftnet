#include "net/acceptor.hpp"
#include "io_context.hpp"
#include <cstring>
#include <errno.h>
#include <iostream>
#include <netinet/in.h>
#include <unistd.h>

using namespace swiftnet::net;

acceptor::acceptor(uint16_t port, int backlog)
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error("socket");

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
        throw std::runtime_error("bind");
    if (listen(listen_fd_, backlog) < 0)
        throw std::runtime_error("listen");

    /* arm multishot accept */
    auto &ctx = swiftnet::io_context::instance();
    for (std::size_t i = 0; i < ctx.rings(); ++i)
    {
        auto &ring = ctx.ring(i);
        auto *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_multishot_accept(sqe, listen_fd_, nullptr, nullptr, 0);
        io_uring_submit(&ring);
    }
}

acceptor::~acceptor() { ::close(listen_fd_); }

vthread acceptor::async_accept(std::function<void(tcp_socket)> cb)
{
    /* completions handled in io_context loop, so just yield forever */
    while (true)
    {
        /* completions will schedule new client coroutines automatically */
        co_await std::suspend_always{};
    }
}
