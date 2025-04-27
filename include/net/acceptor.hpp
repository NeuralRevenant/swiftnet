#ifndef acceptor_hpp
#define acceptor_hpp

#include "../vthread.hpp"
#include "tcp_socket.hpp"

namespace swiftnet::net
{

    class acceptor
    {
    public:
        explicit acceptor(uint16_t port, int backlog = 1024);
        ~acceptor();
        vthread async_accept(std::function<void(tcp_socket)> cb);

    private:
        int listen_fd_;
        void set_nonblock(int fd);
    };

}

#endif
