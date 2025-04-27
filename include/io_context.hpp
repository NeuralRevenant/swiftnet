#ifndef io_context_hpp
#define io_context_hpp

#include <atomic>
#include <liburing.h>
#include <thread>
#include <vector>

namespace swiftnet
{

    class io_context
    {
    public:
        static io_context &instance();

        void start(std::size_t threads = std::thread::hardware_concurrency());
        void stop();

        io_uring &ring(std::size_t idx);

        std::size_t rings() const noexcept { return rings_.size(); }

    private:
        io_context() = default;
        ~io_context();

        void poll_loop(std::size_t idx);

        std::vector<io_uring> rings_;
        std::vector<std::thread> pollers_;
        std::atomic<bool> running_{false};
    };

}

#endif
