#include "vthread.hpp"
#include "vthread_scheduler.hpp"

using namespace swiftnet;

// Implementation for generic vthread_base<T>
template <typename T>
template <typename H>
void vthread_base<T>::promise_type::final_awaitable::await_suspend(H h) noexcept
{
    // Let the scheduler handle cleanup properly instead of destroying directly
    vthread_scheduler::instance().notify_completion(h);
}

// Implementation for vthread_base<void> specialization
template <typename H>
void vthread_base<void>::promise_type::final_awaitable::await_suspend(H h) noexcept
{
    // Let the scheduler handle cleanup properly instead of destroying directly
    vthread_scheduler::instance().notify_completion(h);
}

// Explicit instantiations for common types
template void vthread_base<int>::promise_type::final_awaitable::await_suspend<std::coroutine_handle<vthread_base<int>::promise_type>>(std::coroutine_handle<vthread_base<int>::promise_type>) noexcept;
template void vthread_base<void>::promise_type::final_awaitable::await_suspend<std::coroutine_handle<vthread_base<void>::promise_type>>(std::coroutine_handle<vthread_base<void>::promise_type>) noexcept;
