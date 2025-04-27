#ifndef vthread_scheduler_hpp
#define vthread_scheduler_hpp

#include "detail/mpsc_queue.hpp"
#include "vthread.hpp"
#include <atomic>
#include <memory_resource>
#include <pthread.h>
#include <random>
#include <thread>
#include <vector>

namespace swiftnet
{

    class vthread_scheduler
    {
    public:
        static vthread_scheduler &instance();

        void start(std::size_t threads = std::thread::hardware_concurrency());
        void stop();

        void schedule(vthread t);
        void add_pending(std::coroutine_handle<> h);
        void complete_pending(std::coroutine_handle<> h);
        void notify_completion(std::coroutine_handle<> h);

        std::pmr::memory_resource *local_resource(std::size_t core);

    private:
        vthread_scheduler() = default;
        ~vthread_scheduler();

        void worker(std::size_t core_id);
        void bind_core(std::size_t core);

        using queue_t = detail::mpsc_queue<vthread>;
        std::vector<queue_t> queues_;
        std::vector<std::pmr::monotonic_buffer_resource> arenas_;
        std::vector<std::thread> workers_;
        std::atomic<std::size_t> next_core_{0};
        std::atomic<bool> running_{false};
        std::size_t ncores_{0};
    };

}

#endif
