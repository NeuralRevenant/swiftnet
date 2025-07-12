#ifndef io_context_hpp
#define io_context_hpp

#include "detail/os_backend.hpp"
#include <atomic>
#include <thread>
#include <vector>

#if defined(SWIFTNET_BACKEND_IOURING)
#include <liburing.h>
#endif

namespace swiftnet
{

    class io_context
    {
    public:
        static io_context &instance();
        ~io_context();

        void start(std::size_t threads = std::thread::hardware_concurrency());
        void stop();

#if defined(SWIFTNET_BACKEND_IOURING)
        io_uring &ring(std::size_t idx);
        std::size_t rings() const { return rings_.size(); }
#else
        std::size_t rings() const { return 0; }
#endif

    private:
        io_context() = default;
        void poll_loop(std::size_t idx);

#if defined(SWIFTNET_BACKEND_IOURING)
        std::vector<io_uring> rings_;
#endif
        std::vector<std::thread> pollers_;
        std::atomic<bool> running_{false};
    };

}

#endif
