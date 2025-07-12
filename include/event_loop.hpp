#pragma once
#include "detail/os_backend.hpp"
#include <cstdint>

namespace swiftnet
{

    struct io_event
    {
        int fd;
        std::uint32_t mask; // combination of event_mask values
        int res = 0;        // result code for io_uring/IOCP
    };

    enum event_mask : std::uint32_t
    {
        READABLE = 1u << 0,
        WRITABLE = 1u << 1
    };

    class event_loop
    {
    public:
        event_loop();
        ~event_loop();

        void add(int fd, std::uint32_t mask);
        void mod(int fd, std::uint32_t mask);
        void del(int fd);
        int wait(io_event *ev, int max, int timeout_ms);

    private:
#if defined(SWIFTNET_BACKEND_IOURING)
        struct io_uring *ring_;
#elif defined(SWIFTNET_BACKEND_KQUEUE)
        int kq_;
#elif defined(SWIFTNET_BACKEND_IOCP)
        void *iocp_;
#endif
    };

}
