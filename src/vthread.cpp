#include "vthread.hpp"
#include "vthread_scheduler.hpp"

using namespace swiftnet;

template <typename H>
void vthread::promise_type::final_awaitable::await_suspend(H h) noexcept
{
    vthread_scheduler::instance().notify_completion(h);
}
