#include "vthread_scheduler.hpp"
#include <iostream>

using namespace swiftnet;

vthread_scheduler &vthread_scheduler::instance()
{
    static vthread_scheduler inst;
    return inst;
}

vthread_scheduler::~vthread_scheduler() { stop(); }

void vthread_scheduler::start(std::size_t threads)
{
    if (running_)
        return;
    ncores_ = threads ? threads : 1;
    queues_.resize(ncores_);
    arenas_.reserve(ncores_);
    for (std::size_t i = 0; i < ncores_; ++i)
        arenas_.emplace_back(1024 * 1024); // 1 MiB per-core bump allocator
    running_ = true;
    workers_.reserve(ncores_);
    for (std::size_t i = 0; i < ncores_; ++i)
        workers_.emplace_back([this, i]
                              { worker(i); });
    std::cerr << "[SwiftNet] scheduler online with " << ncores_ << " cores\n";
}

void vthread_scheduler::stop()
{
    running_ = false;
    for (auto &t : workers_)
        if (t.joinable())
            t.join();
    workers_.clear();
}

void vthread_scheduler::bind_core(std::size_t c)
{
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(c, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#endif
}

void vthread_scheduler::worker(std::size_t core)
{
    bind_core(core);
    std::mt19937 rng{static_cast<uint32_t>(core * 7919 + 17)};
    while (running_)
    {
        vthread task;
        if (queues_[core].pop(task))
        {
            if (!task.is_done())
                task.resume();
            continue;
        }
        /* try steal */
        bool stolen = false;
        for (int i = 0; i < 4 && !stolen; ++i)
        {
            auto vic = rng() % ncores_;
            if (queues_[vic].pop(task))
            {
                stolen = true;
                if (!task.is_done())
                    task.resume();
            }
        }
        if (!stolen)
            std::this_thread::yield();
    }
}

void vthread_scheduler::schedule(vthread t)
{
    auto idx = next_core_++ % ncores_;
    queues_[idx].push(std::move(t));
}

void vthread_scheduler::add_pending(std::coroutine_handle<> h)
{
    /* store only handle to wake; attach dummy vthread wrapper */
    schedule(vthread(h));
}

void vthread_scheduler::complete_pending(std::coroutine_handle<> h)
{
    schedule(vthread(h));
}

void vthread_scheduler::notify_completion(std::coroutine_handle<> h) noexcept
{
    h.destroy(); // safe: final_suspend reached
}

std::pmr::memory_resource *vthread_scheduler::local_resource(std::size_t core)
{
    return &arenas_[core];
}
