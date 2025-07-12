#include "event_loop.hpp"
#include <unordered_map>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <errno.h>

#if defined(SWIFTNET_BACKEND_IOURING)
#include <fcntl.h>
#include <poll.h>
#include <liburing.h>
#elif defined(SWIFTNET_BACKEND_KQUEUE)
#include <sys/event.h>
#include <unistd.h>
#elif defined(SWIFTNET_BACKEND_IOCP)
#include <winsock2.h>
#include <windows.h>
#endif

using namespace swiftnet;

/* -----------------------------------------------------------
 * Common helpers
 * ---------------------------------------------------------*/
namespace
{
    /* translate swiftnet::event_mask into OS specific flags */
#if defined(SWIFTNET_BACKEND_IOURING)
    static unsigned to_poll_events(std::uint32_t mask)
    {
        unsigned ev = 0;
        if (mask & READABLE)
            ev |= POLLIN;
        if (mask & WRITABLE)
            ev |= POLLOUT;
        return ev;
    }
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    static int to_kqueue_filter(std::uint32_t mask)
    {
        // not used – we register separate events for read / write.
        (void)mask;
        return 0;
    }
#endif
} // namespace

/* -----------------------------------------------------------
 * Constructor / destructor
 * ---------------------------------------------------------*/

event_loop::event_loop()
{
#if defined(SWIFTNET_BACKEND_IOURING)
    ring_ = new io_uring;
    if (io_uring_queue_init(1024, ring_, IORING_SETUP_SINGLE_ISSUER) != 0)
        throw std::runtime_error("io_uring_queue_init failed");
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    kq_ = kqueue();
    if (kq_ == -1)
        throw std::runtime_error("kqueue() failed");
#elif defined(SWIFTNET_BACKEND_IOCP)
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!iocp_)
        throw std::runtime_error("CreateIoCompletionPort failed");
#endif
}

event_loop::~event_loop()
{
#if defined(SWIFTNET_BACKEND_IOURING)
    io_uring_queue_exit(ring_);
    delete ring_;
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    close(kq_);
#elif defined(SWIFTNET_BACKEND_IOCP)
    CloseHandle(static_cast<HANDLE>(iocp_));
#endif
}

/* -----------------------------------------------------------
 * Add / Mod / Del
 * ---------------------------------------------------------*/

void event_loop::add(int fd, std::uint32_t mask)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    auto sqe = io_uring_get_sqe(ring_);
    io_uring_prep_poll_add(sqe, fd, to_poll_events(mask));
    // store fd in user_data so we can map later
    io_uring_sqe_set_data64(sqe, static_cast<std::uint64_t>(fd));
    io_uring_submit(ring_);
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    struct kevent ev[2];
    int n = 0;
    if (mask & READABLE)
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (mask & WRITABLE)
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    if (kevent(kq_, ev, n, nullptr, 0, nullptr) == -1)
        throw std::runtime_error("kevent add failed");
#elif defined(SWIFTNET_BACKEND_IOCP)
    // IOCP is completion based; user must have issued async I/O.
    // Associate socket/handle so we can poll completions.
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd)), static_cast<HANDLE>(iocp_), static_cast<ULONG_PTR>(fd), 0))
        throw std::runtime_error("CreateIoCompletionPort associate failed");
    (void)mask; // mask not used
#endif
}

void event_loop::mod(int fd, std::uint32_t mask)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    // simply remove and re-add
    del(fd);
    add(fd, mask);
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    del(fd);
    add(fd, mask);
#else
    (void)fd;
    (void)mask;
#endif
}

void event_loop::del(int fd)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    auto sqe = io_uring_get_sqe(ring_);
    io_uring_prep_poll_remove(sqe, static_cast<std::uint64_t>(fd));
    io_uring_submit(ring_);
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kq_, ev, 2, nullptr, 0, nullptr);
#elif defined(SWIFTNET_BACKEND_IOCP)
    // nothing to do – completions will stop arriving when handle closed
    (void)fd;
#endif
}

/* -----------------------------------------------------------
 * Wait
 * ---------------------------------------------------------*/

int event_loop::wait(io_event *evs, int max, int timeout_ms)
{
#if defined(SWIFTNET_BACKEND_IOURING)
    int cnt = 0;
    struct __kernel_timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000L;

    io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe_timeout(ring_, &cqe, &ts);
    if (ret == -ETIME || ret == -EAGAIN)
        return 0;
    if (ret < 0)
        throw std::runtime_error("io_uring_wait_cqe_timeout failed");

    /* iterate over completions up to max */
    while (cqe && cnt < max)
    {
        evs[cnt].fd = static_cast<int>(io_uring_cqe_get_data64(cqe));
        evs[cnt].mask = 0;
        if (cqe->res & POLLIN)
            evs[cnt].mask |= READABLE;
        if (cqe->res & POLLOUT)
            evs[cnt].mask |= WRITABLE;
        evs[cnt].res = cqe->res;
        ++cnt;
        io_uring_cqe_seen(ring_, cqe);
        if (cnt < max)
            io_uring_peek_cqe(ring_, &cqe);
        else
            break;
    }
    return cnt;
#elif defined(SWIFTNET_BACKEND_KQUEUE)
    std::vector<struct kevent> events(max);
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
    int n = kevent(kq_, nullptr, 0, events.data(), max, &ts);
    if (n == -1)
        throw std::runtime_error("kevent wait failed");
    for (int i = 0; i < n; ++i)
    {
        evs[i].fd = static_cast<int>(events[i].ident);
        evs[i].mask = 0;
        if (events[i].filter == EVFILT_READ)
            evs[i].mask |= READABLE;
        if (events[i].filter == EVFILT_WRITE)
            evs[i].mask |= WRITABLE;
        evs[i].res = static_cast<int>(events[i].data);
    }
    return n;
#elif defined(SWIFTNET_BACKEND_IOCP)
    DWORD bytes_transferred = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED overlapped = nullptr;
    BOOL ok = GetQueuedCompletionStatus(static_cast<HANDLE>(iocp_), &bytes_transferred, &key, &overlapped, timeout_ms);
    if (!ok && overlapped == nullptr)
        return 0; // timeout or error with no completion
    if (!ok)
        throw std::runtime_error("GetQueuedCompletionStatus failed");
    evs[0].fd = static_cast<int>(key);
    evs[0].mask = READABLE | WRITABLE; // unknown – assume both
    evs[0].res = static_cast<int>(bytes_transferred);
    return 1;
#endif
} 