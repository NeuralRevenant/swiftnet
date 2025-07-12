#include "io_context.hpp"
#include "io_awaitable.hpp"
#include <iostream>

using namespace swiftnet;

io_context &io_context::instance()
{
    static io_context ctx;
    return ctx;
}

io_context::~io_context() { stop(); }

void io_context::start(std::size_t threads)
{
    if (running_)
        return;
    running_ = true;
    
#if defined(SWIFTNET_BACKEND_IOURING)
    rings_.resize(threads);
    for (std::size_t i = 0; i < threads; ++i)
    {
        if (io_uring_queue_init(1024, &rings_[i], IORING_SETUP_SINGLE_ISSUER))
            throw std::runtime_error("io_uring init failed");
        pollers_.emplace_back([this, i]
                              { poll_loop(i); });
    }
#else
    // For non-uring backends, create minimal poll threads
    for (std::size_t i = 0; i < threads; ++i)
    {
        pollers_.emplace_back([this, i]
                              { poll_loop(i); });
    }
#endif
}

void io_context::stop()
{
    running_ = false;
    for (auto &p : pollers_)
        if (p.joinable())
            p.join();
            
#if defined(SWIFTNET_BACKEND_IOURING)
    for (auto &r : rings_)
        io_uring_queue_exit(&r);
    rings_.clear();
#endif
    pollers_.clear();
}

void io_context::poll_loop(std::size_t idx)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    auto &ring = rings_[idx];
    while (running_)
    {
        io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret == 0)
        {
            auto *h = reinterpret_cast<std::coroutine_handle<> *>(io_uring_cqe_get_data(cqe));
            if (h)
                io_awaitable::complete(h, cqe->res);
            io_uring_cqe_seen(&ring, cqe);
        }
    }
#else
    // Simple poll loop for non-uring backends
    while (running_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
}

#if defined(SWIFTNET_BACKEND_IOURING)
io_uring &io_context::ring(std::size_t idx) { return rings_[idx]; }
#endif
