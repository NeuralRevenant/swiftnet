#ifndef vthread_hpp
#define vthread_hpp

#include <coroutine>

namespace swiftnet
{

    class vthread
    {
    public:
        struct promise_type
        {
            vthread get_return_object() noexcept
            {
                return vthread{handle_type::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct final_awaitable
            {
                bool await_ready() const noexcept { return false; }

                template <typename H>
                void await_suspend(H h) noexcept;

                void await_resume() noexcept {}
            };

            final_awaitable final_suspend() noexcept { return {}; }

            void unhandled_exception() { std::terminate(); }

            void return_void() noexcept {}
        };

        using handle_type = std::coroutine_handle<promise_type>;

        explicit vthread(handle_type h = {}) : coro_(h) {}

        vthread(vthread &&o) noexcept : coro_(o.coro_) { o.coro_ = {}; }

        vthread &operator=(vthread &&o) noexcept
        {
            if (&o != this)
            {
                if (coro_)
                    coro_.destroy();
                coro_ = o.coro_;
                o.coro_ = {};
            }
            return *this;
        }

        ~vthread()
        {
            if (coro_)
                coro_.destroy();
        }

        void resume()
        {
            if (coro_ && !coro_.done())
                coro_.resume();
        }

        [[nodiscard]] bool is_done() const { return !coro_ || coro_.done(); }

        handle_type handle() const { return coro_; }

    private:
        handle_type coro_;
    };

}

#endif
