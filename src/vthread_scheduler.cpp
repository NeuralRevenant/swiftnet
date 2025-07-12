#include "vthread_scheduler.hpp"
#include "event_loop.hpp"
#include "io_context.hpp"
#include <iostream>
#include <algorithm>
#include <cassert>

using namespace swiftnet;

vthread_scheduler &vthread_scheduler::instance()
{
    static vthread_scheduler inst;
    return inst;
}

vthread_scheduler::~vthread_scheduler() 
{ 
    stop(); 
}

void vthread_scheduler::start(std::size_t threads)
{
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (running_)
        return;
    
    ncores_ = threads ? threads : std::thread::hardware_concurrency();
    
    // Initialize core data structures
    queues_.resize(ncores_);
    arenas_.reserve(ncores_);
    core_loads_.resize(ncores_);
    worker_conditions_.resize(ncores_);
    worker_mutexes_.resize(ncores_);
    worker_sleeping_.resize(ncores_, false);
    
    // Initialize per-core memory arenas
    for (std::size_t i = 0; i < ncores_; ++i) {
        arenas_.emplace_back(std::make_unique<std::pmr::monotonic_buffer_resource>(1024 * 1024)); // 1 MiB per-core
        core_loads_[i] = std::make_unique<std::atomic<uint32_t>>(0);
        worker_conditions_[i] = std::make_unique<std::condition_variable>();
        worker_mutexes_[i] = std::make_unique<std::mutex>();
    }
    
    // Initialize statistics
    stats_.per_core_executed.resize(ncores_, 0);
    
    // Start event loop and I/O context
    event_loop_ = std::make_unique<event_loop>();
    io_context_ = std::shared_ptr<io_context>(&io_context::instance(), [](io_context*){});
    
    running_ = true;
    cleanup_running_ = true;
    
    // Start worker threads
    workers_.reserve(ncores_);
    for (std::size_t i = 0; i < ncores_; ++i) {
        workers_.emplace_back([this, i] { worker(i); });
    }
    
    // Start cleanup thread
    cleanup_thread_ = std::thread([this] { 
        while (cleanup_running_) {
            cleanup_expired_io_operations();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    
    last_balance_time_ = std::chrono::steady_clock::now();
    
    std::cerr << "[SwiftNet] Advanced scheduler online with " << ncores_ << " cores\n";
}

void vthread_scheduler::stop()
{
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (!running_)
        return;
        
    running_ = false;
    cleanup_running_ = false;
    
    // Wake up all sleeping workers
    for (std::size_t i = 0; i < ncores_; ++i) {
        wake_worker(i);
    }
    
    // Join all worker threads
    for (auto &t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    // Join cleanup thread
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
    
    // Clean up remaining I/O operations
    {
        std::lock_guard<std::mutex> io_lock(io_operations_mutex_);
        for (auto& [handle, op] : io_operations_) {
            if (handle && !handle.done()) {
                handle.destroy();
            }
        }
        io_operations_.clear();
    }
    
    // Clean up virtual thread contexts
    {
        std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
        vthread_contexts_.clear();
    }
    
    // Clean up resources
    workers_.clear();
    queues_.clear();
    arenas_.clear();
    core_loads_.clear();
    worker_conditions_.clear();
    worker_mutexes_.clear();
    worker_sleeping_.clear();
    
    event_loop_.reset();
    io_context_.reset();
    
    std::cerr << "[SwiftNet] Advanced scheduler stopped\n";
}

void vthread_scheduler::bind_core(std::size_t c)
{
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(c, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#elif defined(__APPLE__)
    // macOS doesn't support CPU affinity in the same way
    // We can use thread_policy_set but it's more complex
    (void)c;
#endif
}

void vthread_scheduler::worker(std::size_t core)
{
    bind_core(core);
    
    std::mt19937 rng{static_cast<uint32_t>(core * 7919 + 17)};
    auto last_balance_check = std::chrono::steady_clock::now();
    
    while (running_) {
        bool found_work = false;
        
        // Try to get work from local queue
        vthread task;
        if (queues_[core].pop(task)) {
            found_work = true;
            
            if (task.valid() && !task.is_done()) {
                // Mount the virtual thread
                mount_vthread(task.handle(), core);
                
                // Execute the virtual thread
                auto suspend_reason = execute_vthread(task.handle());
                
                // Handle suspension reason
                switch (suspend_reason) {
                    case SuspendReason::NONE:
                        // Continue execution
                        if (!task.is_done()) {
                            queues_[core].push(std::move(task));
                        }
                        break;
                        
                    case SuspendReason::IO_WAIT:
                        // Virtual thread is suspended for I/O - don't reschedule
                        break;
                        
                    case SuspendReason::YIELD:
                        // Reschedule to a potentially different core
                        schedule(std::move(task));
                        break;
                        
                    case SuspendReason::COMPLETED:
                        // Virtual thread completed - cleanup already handled by notify_completion
                        // Just decrease the core load
                        core_loads_[core]->fetch_sub(1, std::memory_order_relaxed);
                        break;
                        
                    case SuspendReason::PREEMPTED:
                        // Reschedule immediately
                        queues_[core].push(std::move(task));
                        break;
                }
                
                // Update statistics
                {
                    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                    stats_.per_core_executed[core]++;
                    stats_.context_switches++;
                }
            }
        }
        
        // Try work stealing if no local work found
        if (!found_work) {
            found_work = try_steal_work(core);
        }
        
        // Periodic load balancing
        auto now = std::chrono::steady_clock::now();
        if (now - last_balance_check > std::chrono::milliseconds(50)) {
            balance_load();
            last_balance_check = now;
        }
        
        // Sleep if no work found
        if (!found_work) {
            sleep_worker(core);
        }
    }
    
    std::cerr << "[SwiftNet] Worker " << core << " shutting down\n";
}

bool vthread_scheduler::try_steal_work(std::size_t core)
{
    std::mt19937 rng{static_cast<uint32_t>(core * 7919 + 17)};
    
    // Try to steal from 4 random cores
    for (int attempts = 0; attempts < 4; ++attempts) {
        std::size_t victim = rng() % ncores_;
        if (victim == core) continue;
        
        vthread task;
        if (queues_[victim].pop(task)) {
            if (task.valid() && !task.is_done()) {
                // Successfully stole work
                mount_vthread(task.handle(), core);
                auto suspend_reason = execute_vthread(task.handle());
                
                // Handle stolen work
                switch (suspend_reason) {
                    case SuspendReason::NONE:
                        if (!task.is_done()) {
                            queues_[core].push(std::move(task));
                        } else {
                            // Thread completed during execution - cleanup handled by notify_completion
                            core_loads_[core]->fetch_sub(1, std::memory_order_relaxed);
                        }
                        break;
                    case SuspendReason::IO_WAIT:
                        break;
                    case SuspendReason::YIELD:
                        schedule(std::move(task));
                        break;
                    case SuspendReason::COMPLETED:
                        // Virtual thread completed - cleanup already handled by notify_completion
                        core_loads_[core]->fetch_sub(1, std::memory_order_relaxed);
                        break;
                    case SuspendReason::PREEMPTED:
                        queues_[core].push(std::move(task));
                        break;
                }
                
                // Update statistics
                {
                    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                    stats_.work_stolen++;
                    stats_.per_core_executed[core]++;
                }
                
                return true;
            }
        }
    }
    
    return false;
}

void vthread_scheduler::wake_worker(std::size_t core)
{
    std::lock_guard<std::mutex> lock(*worker_mutexes_[core]);
    if (worker_sleeping_[core]) {
        worker_sleeping_[core] = false;
        worker_conditions_[core]->notify_one();
    }
}

void vthread_scheduler::sleep_worker(std::size_t core)
{
    std::unique_lock<std::mutex> lock(*worker_mutexes_[core]);
    worker_sleeping_[core] = true;
    
    // Wait for work or shutdown
    worker_conditions_[core]->wait_for(lock, std::chrono::milliseconds(10), [this, core] {
        return !running_ || !worker_sleeping_[core];
    });
}

void vthread_scheduler::schedule(vthread t)
{
    if (!running_) return;
    
    auto core = select_best_core();
    queues_[core].push(std::move(t));
    core_loads_[core]->fetch_add(1, std::memory_order_relaxed);
    
    // Wake up the worker if it's sleeping
    wake_worker(core);
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_scheduled++;
    }
}

void vthread_scheduler::schedule_with_affinity(vthread t, std::size_t preferred_core)
{
    if (!running_) return;
    
    std::size_t core = std::min(preferred_core, ncores_ - 1);
    queues_[core].push(std::move(t));
    core_loads_[core]->fetch_add(1, std::memory_order_relaxed);
    
    wake_worker(core);
    
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_scheduled++;
    }
}

void vthread_scheduler::yield_current(std::coroutine_handle<> h)
{
    if (!h || h.done()) return;
    
    std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
    auto it = vthread_contexts_.find(h);
    if (it != vthread_contexts_.end()) {
        it->second.suspend_reason = SuspendReason::YIELD;
    }
}

void vthread_scheduler::suspend_for_io(std::coroutine_handle<> h, int fd, uint32_t events)
{
    if (!h || h.done()) return;
    
    // Add to I/O operations tracking
    {
        std::lock_guard<std::mutex> io_lock(io_operations_mutex_);
        io_operations_.emplace(h, IOOperation{fd, events, h});
    }
    
    // Update virtual thread context
    {
        std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
        auto it = vthread_contexts_.find(h);
        if (it != vthread_contexts_.end()) {
            it->second.suspend_reason = SuspendReason::IO_WAIT;
        }
    }
    
    // Register with event loop for I/O completion
    if (event_loop_) {
        event_loop_->add(fd, events);
    }
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_io_suspended++;
    }
}

void vthread_scheduler::resume_from_io(std::coroutine_handle<> h, int result)
{
    std::cout << "[DEBUG] resume_from_io called! handle=" << (void*)h.address() << " result=" << result << std::endl;
    
    if (!h) {
        std::cout << "[DEBUG] resume_from_io: handle is NULL, returning" << std::endl;
        return;
    }
    
    if (h.done()) {
        std::cout << "[DEBUG] resume_from_io: handle is DONE (coroutine completed), returning" << std::endl;
        return;
    }
    
    std::cout << "[DEBUG] resume_from_io: handle is valid and not done, proceeding..." << std::endl;
    
    std::cout << "[DEBUG] resume_from_io: removing from I/O operations..." << std::endl;
    
    // Remove from I/O operations tracking
    {
        std::lock_guard<std::mutex> io_lock(io_operations_mutex_);
        auto it = io_operations_.find(h);
        if (it != io_operations_.end()) {
            if (event_loop_) {
                event_loop_->del(it->second.fd);
            }
            io_operations_.erase(it);
            std::cout << "[DEBUG] resume_from_io: removed from I/O operations" << std::endl;
        } else {
            std::cout << "[DEBUG] resume_from_io: handle not found in I/O operations!" << std::endl;
        }
    }
    
    std::cout << "[DEBUG] resume_from_io: updating context..." << std::endl;
    
    // Update virtual thread context to show it's no longer waiting for I/O
    {
        std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
        auto it = vthread_contexts_.find(h);
        if (it != vthread_contexts_.end()) {
            it->second.suspend_reason = SuspendReason::NONE;
            std::cout << "[DEBUG] resume_from_io: updated context suspend reason" << std::endl;
        } else {
            std::cout << "[DEBUG] resume_from_io: context not found!" << std::endl;
        }
    }
    
    std::cout << "[DEBUG] resume_from_io: scheduling coroutine through scheduler..." << std::endl;
    
    // CRITICAL FIX: Instead of calling h.resume() directly from background thread,
    // schedule the coroutine through the scheduler to avoid deadlocks
    if (!h.done()) {
        try {
            vthread resumed_vthread = vthread::from_handle(h);
            schedule(std::move(resumed_vthread));
            std::cout << "[DEBUG] resume_from_io: coroutine scheduled successfully" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "[DEBUG] resume_from_io: exception during scheduling: " << e.what() << std::endl;
        }
    } else {
        std::cout << "[DEBUG] resume_from_io: handle became done during processing!" << std::endl;
    }
    
    // Update statistics
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_resumed++;
        std::cout << "[DEBUG] resume_from_io: statistics updated, total_resumed=" << stats_.total_resumed << std::endl;
    }
}

void vthread_scheduler::cancel_io_operation(std::coroutine_handle<> h)
{
    if (!h) return;
    
    std::lock_guard<std::mutex> io_lock(io_operations_mutex_);
    auto it = io_operations_.find(h);
    if (it != io_operations_.end()) {
        if (event_loop_) {
            event_loop_->del(it->second.fd);
        }
        io_operations_.erase(it);
    }
}

void vthread_scheduler::mount_vthread(std::coroutine_handle<> h, std::size_t core)
{
    if (!h) return;
    
    std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
    auto it = vthread_contexts_.find(h);
    if (it == vthread_contexts_.end()) {
        // Create new context
        vthread_contexts_.emplace(h, VThreadContext{h});
        it = vthread_contexts_.find(h);
    }
    
    it->second.is_mounted = true;
    it->second.core_affinity = core;
    it->second.last_resume = std::chrono::steady_clock::now();
    it->second.suspend_reason = SuspendReason::NONE;
}

void vthread_scheduler::unmount_vthread(std::coroutine_handle<> h, std::size_t core)
{
    if (!h) return;
    
    std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
    auto it = vthread_contexts_.find(h);
    if (it != vthread_contexts_.end()) {
        it->second.is_mounted = false;
        
        // Update CPU time
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            now - it->second.last_resume
        );
        it->second.cpu_time_us += duration.count();
        
        // Remove context if completed (but this should already be handled by notify_completion)
        if (it->second.suspend_reason == SuspendReason::COMPLETED) {
            vthread_contexts_.erase(it);
        }
    }
    
    // Decrease core load
    if (core < core_loads_.size()) {
    core_loads_[core]->fetch_sub(1, std::memory_order_relaxed);
    }
}

SuspendReason vthread_scheduler::execute_vthread(std::coroutine_handle<> h)
{
    if (!h || h.done()) {
        return SuspendReason::COMPLETED;
    }
    
    // Check if we should preempt based on execution time
    {
        std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
        auto it = vthread_contexts_.find(h);
        if (it != vthread_contexts_.end() && should_preempt_vthread(it->second)) {
            return SuspendReason::PREEMPTED;
        }
    }
    
    // Execute the coroutine
    try {
        h.resume();
        
        if (h.done()) {
            return SuspendReason::COMPLETED;
        }
        
        // Check suspension reason
        std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
        auto it = vthread_contexts_.find(h);
        if (it != vthread_contexts_.end()) {
            return it->second.suspend_reason;
        }
        
        return SuspendReason::NONE;
    } catch (const std::exception& e) {
        std::cerr << "[SwiftNet] Exception in vthread: " << e.what() << std::endl;
        return SuspendReason::COMPLETED;
    }
}

std::size_t vthread_scheduler::select_best_core() const
{
    std::size_t best_core = 0;
    uint32_t min_load = core_loads_[0]->load(std::memory_order_relaxed);
    
    for (std::size_t i = 1; i < ncores_; ++i) {
        uint32_t load = core_loads_[i]->load(std::memory_order_relaxed);
        if (load < min_load) {
            min_load = load;
            best_core = i;
        }
    }
    
    return best_core;
}

void vthread_scheduler::balance_load()
{
    // Simple load balancing - move work from overloaded cores to underloaded ones
    auto now = std::chrono::steady_clock::now();
    if (now - last_balance_time_ < std::chrono::milliseconds(100)) {
        return;
    }
    
    last_balance_time_ = now;
    
    // Find the most and least loaded cores
    std::size_t max_core = 0, min_core = 0;
    uint32_t max_load = 0, min_load = UINT32_MAX;
    
    for (std::size_t i = 0; i < ncores_; ++i) {
        uint32_t load = core_loads_[i]->load(std::memory_order_relaxed);
        if (load > max_load) {
            max_load = load;
            max_core = i;
        }
        if (load < min_load) {
            min_load = load;
            min_core = i;
        }
    }
    
    // If difference is significant, try to steal work
    if (max_load > min_load + 2) {
        vthread task;
        if (queues_[max_core].pop(task)) {
            queues_[min_core].push(std::move(task));
            core_loads_[max_core]->fetch_sub(1, std::memory_order_relaxed);
            core_loads_[min_core]->fetch_add(1, std::memory_order_relaxed);
            wake_worker(min_core);
        }
    }
}

bool vthread_scheduler::should_preempt_vthread(const VThreadContext& ctx) const
{
    // Preempt if vthread has been running for more than 10ms
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ctx.last_resume
    );
    return duration.count() > 10;
}

void vthread_scheduler::cleanup_expired_io_operations()
{
    auto now = std::chrono::steady_clock::now();
    std::vector<std::coroutine_handle<>> expired;
    
    {
        std::lock_guard<std::mutex> io_lock(io_operations_mutex_);
        for (auto it = io_operations_.begin(); it != io_operations_.end(); ) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.start_time
            );
            
            // Clean up operations that have been waiting for more than 30 seconds
            if (duration.count() > 30) {
                expired.push_back(it->first);
                it = io_operations_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Clean up expired operations
    for (auto h : expired) {
        if (h && !h.done()) {
            h.destroy();
        }
    }
}

auto vthread_scheduler::get_stats() const -> Stats
{
    std::lock_guard<std::mutex> stats_lock(stats_mutex_);
    return stats_;
}

// Legacy interface implementations (for backward compatibility)
void vthread_scheduler::add_pending(std::coroutine_handle<> h)
{
    if (!h || h.done()) return;
    
    // Resume directly without creating wrapper to avoid double ownership
    if (!h.done()) {
        h.resume();
    }
}

void vthread_scheduler::complete_pending(std::coroutine_handle<> h)
{
    if (!h || h.done()) return;
    
    // Resume directly without creating wrapper to avoid double ownership
    if (!h.done()) {
        h.resume();
    }
}

void vthread_scheduler::notify_completion(std::coroutine_handle<> h) noexcept
{
    if (!h) return;
    
    try {
        {
        std::lock_guard<std::mutex> ctx_lock(contexts_mutex_);
        auto it = vthread_contexts_.find(h);
        if (it != vthread_contexts_.end()) {
            it->second.suspend_reason = SuspendReason::COMPLETED;
                // Clean up the context immediately for completed threads
                vthread_contexts_.erase(it);
            }
        }
        
        // DON'T destroy here - let the vthread destructor handle it
        // This prevents double free errors
    } catch (...) {
        // Ignore exceptions in noexcept function
    }
}

std::pmr::memory_resource *vthread_scheduler::local_resource(std::size_t core)
{
    if (core >= arenas_.size())
        return std::pmr::get_default_resource();
    return arenas_[core].get();
}
