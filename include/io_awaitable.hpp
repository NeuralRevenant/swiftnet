#ifndef io_awaitable_hpp
#define io_awaitable_hpp

#include "detail/os_backend.hpp"
#include <coroutine>

#if defined(SWIFTNET_BACKEND_IOURING)
#include <liburing.h>
#endif

namespace swiftnet
{

    class io_awaitable
    {
    public:
        io_awaitable(int fd, unsigned poll_events, bool oneshot = true);

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h);
        int await_resume();

    private:
        int fd_;
        unsigned events_;
        bool oneshot_;
        int res_{0};
        std::coroutine_handle<> handle_;

        // New simplified I/O polling implementation
        bool check_immediate_availability();
        void start_io_polling();
    };

}

#endif
