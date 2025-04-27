#ifndef io_awaitable_hpp
#define io_awaitable_hpp

#include <coroutine>
#include <liburing.h>

namespace swiftnet
{

    class io_awaitable
    {
    public:
        io_awaitable(int fd, unsigned poll_events, bool oneshot = true);

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h);
        int await_resume() noexcept { return res_; }

        /* completion from poll loop */
        static void complete(std::coroutine_handle<> *h_ptr, int res);

    private:
        int fd_;
        unsigned events_;
        bool oneshot_;
        int res_{0};
    };

}

#endif
