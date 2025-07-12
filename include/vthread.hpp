#ifndef vthread_hpp
#define vthread_hpp

#include <coroutine>
#include <exception>

namespace swiftnet
{

    template<typename T = void>
    class vthread_base
    {
    public:
        struct promise_type
        {
            T result_;

            auto get_return_object() noexcept
            {
                return vthread_base{handle_type::from_promise(*this)};
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

            // For non-void return type only
            void return_value(T value) { result_ = std::move(value); }
        };

        using handle_type = std::coroutine_handle<promise_type>;

        // Default constructor for safe empty state
        vthread_base() noexcept : coro_{} {}

        explicit vthread_base(handle_type h) : coro_(h) {}

        vthread_base(vthread_base &&o) noexcept : coro_(o.coro_) { o.coro_ = {}; }

        vthread_base &operator=(vthread_base &&o) noexcept
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

        ~vthread_base()
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

        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(coro_); }

        handle_type handle() const { return coro_; }

        // Get result for non-void types
        T result() const { return coro_.promise().result_; }

        // Awaiter interface
        bool await_ready() const noexcept 
        { 
            return !coro_ || coro_.done(); 
        }

        template<typename H>
        void await_suspend(H) noexcept 
        {
            // Schedule the coroutine to run
            if (coro_ && !coro_.done()) {
                resume();
            }
        }

        T await_resume() 
        { 
            if constexpr (!std::is_void_v<T>) {
                return result();
            }
        }

        // Factory method to create vthread from handle
        static vthread_base from_handle(handle_type h) 
        {
            return vthread_base{h};
        }

    private:
        handle_type coro_;
    };

    // Template specialization for void
    template<>
    class vthread_base<void>
    {
    public:
        struct promise_type
        {
            auto get_return_object() noexcept
            {
                return vthread_base{handle_type::from_promise(*this)};
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

            // For void return type only
            void return_void() noexcept {}
        };

        using handle_type = std::coroutine_handle<promise_type>;

        // Default constructor for safe empty state
        vthread_base() noexcept : coro_{} {}
        
        explicit vthread_base(handle_type h) : coro_(h) {}

        vthread_base(vthread_base &&o) noexcept : coro_(o.coro_) { o.coro_ = {}; }

        vthread_base &operator=(vthread_base &&o) noexcept
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

        ~vthread_base()
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

        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(coro_); }

        handle_type handle() const { return coro_; }

        // Awaiter interface
        bool await_ready() const noexcept 
        { 
            return !coro_ || coro_.done(); 
        }

        template<typename H>
        void await_suspend(H) noexcept 
        {
            // Schedule the coroutine to run
            if (coro_ && !coro_.done()) {
                resume();
            }
        }

        void await_resume() noexcept 
        { 
            // void specialization - nothing to return
        }

        // Factory method to create vthread from handle
        static vthread_base from_handle(handle_type h) 
        {
            return vthread_base{h};
        }

        // Factory method to create vthread from generic coroutine handle
        static vthread_base from_handle(std::coroutine_handle<> h) 
        {
            return vthread_base{handle_type::from_address(h.address())};
        }

    private:
        handle_type coro_;
    };

    using vthread = vthread_base<void>;

}

#endif
