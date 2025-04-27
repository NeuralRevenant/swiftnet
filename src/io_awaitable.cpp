#include "io_awaitable.hpp"
#include "io_context.hpp"
#include "vthread_scheduler.hpp"

using namespace swiftnet;

io_awaitable::io_awaitable(int fd, unsigned poll_events, bool oneshot)
    : fd_(fd), events_(poll_events), oneshot_(oneshot) {}

void io_awaitable::await_suspend(std::coroutine_handle<> h)
{
    static std::atomic<std::size_t> idx{0};
    auto &ctx = io_context::instance();
    auto &ring = ctx.ring(idx++ % ctx.rings());

    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_poll_add(sqe, fd_, events_);
    if (!oneshot_)
        sqe->flags |= IOSQE_IO_LINK;
    auto *handle_ptr = new std::coroutine_handle<>(h);
    io_uring_sqe_set_data(sqe, handle_ptr);
    io_uring_submit(&ring);

    vthread_scheduler::instance().add_pending(h);
}

void io_awaitable::complete(std::coroutine_handle<> *h_ptr, int res)
{
    vthread_scheduler::instance().complete_pending(*h_ptr);
    delete h_ptr;
}
