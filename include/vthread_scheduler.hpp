#ifndef vthread_scheduler_hpp
#define vthread_scheduler_hpp

#include "detail/mpsc_queue.hpp"
#include "vthread.hpp"
#include <atomic>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <pthread.h>
#include <random>
#include <thread>
#include <vector>
#include <unordered_map>
#include <condition_variable>
#include <chrono>

namespace swiftnet
{
    // Forward declarations
    class event_loop;
    class io_context;

    // Suspension reasons for virtual threads
    enum class SuspendReason {
        NONE,
        IO_WAIT,
        YIELD,
        COMPLETED,
        PREEMPTED
    };

    // I/O operation details
    struct IOOperation {
        int fd;
        uint32_t events;
        std::coroutine_handle<> handle;
        std::chrono::steady_clock::time_point start_time;
        
        IOOperation(int f, uint32_t e, std::coroutine_handle<> h)
            : fd(f), events(e), handle(h), start_time(std::chrono::steady_clock::now()) {}
    };

    // Virtual thread execution context
    struct VThreadContext {
        std::coroutine_handle<> handle;
        SuspendReason suspend_reason{SuspendReason::NONE};
        std::chrono::steady_clock::time_point last_resume;
        uint64_t cpu_time_us{0};
        uint32_t core_affinity{0};
        bool is_mounted{false};
        
        VThreadContext(std::coroutine_handle<> h) 
            : handle(h), last_resume(std::chrono::steady_clock::now()) {}
    };

    class vthread_scheduler
    {
    public:
        static vthread_scheduler &instance();

        void start(std::size_t threads = std::thread::hardware_concurrency());
        void stop();

        // Core scheduling operations
        void schedule(vthread t);
        void schedule_with_affinity(vthread t, std::size_t preferred_core);
        void yield_current(std::coroutine_handle<> h);
        
        // I/O suspension/resumption
        void suspend_for_io(std::coroutine_handle<> h, int fd, uint32_t events);
        void resume_from_io(std::coroutine_handle<> h, int result);
        void cancel_io_operation(std::coroutine_handle<> h);
        
        // Virtual thread lifecycle
        void mount_vthread(std::coroutine_handle<> h, std::size_t core);
        void unmount_vthread(std::coroutine_handle<> h, std::size_t core);
        SuspendReason execute_vthread(std::coroutine_handle<> h);
        
        // Legacy interface (deprecated but kept for compatibility)
        void add_pending(std::coroutine_handle<> h);
        void complete_pending(std::coroutine_handle<> h);
        void notify_completion(std::coroutine_handle<> h) noexcept;

        // Resource management
        std::pmr::memory_resource *local_resource(std::size_t core);
        
        // Statistics and monitoring
        struct Stats {
            uint64_t total_scheduled{0};
            uint64_t total_io_suspended{0};
            uint64_t total_resumed{0};
            uint64_t work_stolen{0};
            uint64_t context_switches{0};
            std::vector<uint64_t> per_core_executed;
        };
        Stats get_stats() const;

    private:
        vthread_scheduler() = default;
        ~vthread_scheduler();

        // Worker thread management
        void worker(std::size_t core_id);
        void bind_core(std::size_t core);
        bool try_steal_work(std::size_t core);
        void wake_worker(std::size_t core);
        void sleep_worker(std::size_t core);

        // I/O event handling
        void process_io_completions();
        void cleanup_expired_io_operations();
        
        // Internal scheduling helpers
        std::size_t select_best_core() const;
        void balance_load();
        bool should_preempt_vthread(const VThreadContext& ctx) const;

        using queue_t = detail::mpsc_queue<vthread>;
        
        // Core data structures
        std::vector<queue_t> queues_;
        std::vector<std::unique_ptr<std::pmr::monotonic_buffer_resource>> arenas_;
        std::vector<std::thread> workers_;
        
        // I/O suspension management
        std::unordered_map<std::coroutine_handle<>, IOOperation> io_operations_;
        std::mutex io_operations_mutex_;
        
        // Worker thread synchronization
        std::vector<std::unique_ptr<std::condition_variable>> worker_conditions_;
        std::vector<std::unique_ptr<std::mutex>> worker_mutexes_;
        std::vector<bool> worker_sleeping_;
        
        // Virtual thread context tracking
        std::unordered_map<std::coroutine_handle<>, VThreadContext> vthread_contexts_;
        std::mutex contexts_mutex_;
        
        // Load balancing
        std::atomic<std::size_t> next_core_{0};
        std::vector<std::unique_ptr<std::atomic<uint32_t>>> core_loads_;
        std::chrono::steady_clock::time_point last_balance_time_;
        
        // State management
        std::atomic<bool> running_{false};
        std::size_t ncores_{0};
        std::mutex global_mutex_;
        
        // Statistics
        mutable Stats stats_;
        mutable std::mutex stats_mutex_;
        
        // Timers and cleanup
        std::thread cleanup_thread_;
        std::atomic<bool> cleanup_running_{false};
        
        // Integration with event loop
        std::unique_ptr<event_loop> event_loop_;
        std::shared_ptr<io_context> io_context_;
    };

}

#endif
