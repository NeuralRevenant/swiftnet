#include "io_awaitable.hpp"
#include "io_context.hpp"
#include "vthread_scheduler.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <errno.h>

#if defined(SWIFTNET_BACKEND_KQUEUE)
#include <sys/event.h>
#include <unistd.h>
#elif defined(SWIFTNET_BACKEND_IOCP)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <poll.h>
#endif

// Define POLLIN/POLLOUT constants if not available
#ifndef POLLIN
#define POLLIN 0x001
#endif
#ifndef POLLOUT
#define POLLOUT 0x004
#endif

using namespace swiftnet;

io_awaitable::io_awaitable(int fd, unsigned poll_events, bool oneshot)
    : fd_(fd), events_(poll_events), oneshot_(oneshot) {}

void io_awaitable::await_suspend(std::coroutine_handle<> h)
{
    std::cout << "[DEBUG] io_awaitable::await_suspend fd=" << fd_ << " events=" << events_ << std::endl;
    
    // Suspend the virtual thread for I/O
    vthread_scheduler::instance().suspend_for_io(h, fd_, events_);
    
    // Start background I/O polling to avoid blocking scheduler
    std::cout << "[DEBUG] Starting background I/O polling for fd=" << fd_ << std::endl;
    
    // Store the handle for the background thread 
    handle_ = h;
    
    // CRITICAL FIX: Capture fd and events by value immediately to prevent corruption
    const int capture_fd = fd_;
    const uint32_t capture_events = events_;
    
    std::thread([this, h, capture_fd, capture_events]() {
        std::cout << "[DEBUG] Background thread started for fd=" << capture_fd << " events=" << capture_events << std::endl;
        
        if (capture_fd <= 0) {
            std::cout << "[DEBUG] ERROR: Invalid fd=" << capture_fd << " in background thread!" << std::endl;
            try {
                vthread_scheduler::instance().resume_from_io(h, -1); // error
            } catch (const std::exception& e) {
                std::cout << "[DEBUG] Background thread: EXCEPTION in resume_from_io for invalid fd: " << e.what() << std::endl;
            }
            return;
        }
        
        fd_set readfds, writefds, exceptfds;
        struct timeval timeout;
        
        try {
            for (int i = 0; i < 1000; ++i) {
                FD_ZERO(&readfds);
                FD_ZERO(&writefds);
                FD_ZERO(&exceptfds);
                
                if (capture_events & POLLIN) {
                    FD_SET(capture_fd, &readfds);
                }
                if (capture_events & POLLOUT) {
                    FD_SET(capture_fd, &writefds);
                }
                FD_SET(capture_fd, &exceptfds);
                
                timeout.tv_sec = 0;
                timeout.tv_usec = 10000; // 10ms
                
                int ret = select(capture_fd + 1, &readfds, &writefds, &exceptfds, &timeout);
                
                if (i % 100 == 0) {
                    std::cout << "[DEBUG] Poll " << i << ": ret=" << ret << " for fd=" << capture_fd << std::endl;
                }
                
                if (ret > 0) {
                    std::cout << "[DEBUG] I/O ready on fd=" << capture_fd << std::endl;
                    try {
                        vthread_scheduler::instance().resume_from_io(h, 1); // success
                        std::cout << "[DEBUG] Background thread: resume_from_io called successfully for fd=" << capture_fd << std::endl;
                    } catch (const std::exception& e) {
                        std::cout << "[DEBUG] Background thread: EXCEPTION in resume_from_io: " << e.what() << std::endl;
                    }
                    return;
                } else if (ret < 0) {
                    std::cout << "[DEBUG] select() error on fd=" << capture_fd << ": " << strerror(errno) << std::endl;
                    try {
                        vthread_scheduler::instance().resume_from_io(h, -1); // error
                        std::cout << "[DEBUG] Background thread: resume_from_io called for error on fd=" << capture_fd << std::endl;
                    } catch (const std::exception& e) {
                        std::cout << "[DEBUG] Background thread: EXCEPTION in resume_from_io for error: " << e.what() << std::endl;
                    }
                    return;
                }
                // ret == 0 means timeout, continue polling
            }
            
            std::cout << "[DEBUG] Polling timed out after 1000 attempts for fd=" << capture_fd << std::endl;
            try {
                vthread_scheduler::instance().resume_from_io(h, -2); // timeout
                std::cout << "[DEBUG] Background thread: resume_from_io called for timeout on fd=" << capture_fd << std::endl;
            } catch (const std::exception& e) {
                std::cout << "[DEBUG] Background thread: EXCEPTION in resume_from_io for timeout: " << e.what() << std::endl;
            }
            
        } catch (const std::exception& e) {
            std::cout << "[DEBUG] Background thread: OUTER EXCEPTION on fd=" << capture_fd << ": " << e.what() << std::endl;
            try {
                vthread_scheduler::instance().resume_from_io(h, -3); // exception
            } catch (const std::exception& e2) {
                std::cout << "[DEBUG] Background thread: EXCEPTION in resume_from_io for exception: " << e2.what() << std::endl;
            }
        } catch (...) {
            std::cout << "[DEBUG] Background thread: UNKNOWN EXCEPTION on fd=" << capture_fd << std::endl;
            try {
                vthread_scheduler::instance().resume_from_io(h, -4); // unknown exception
            } catch (const std::exception& e) {
                std::cout << "[DEBUG] Background thread: EXCEPTION in resume_from_io for unknown exception: " << e.what() << std::endl;
            }
        }
    }).detach();
}

bool io_awaitable::check_immediate_availability()
{
    fd_set read_fds, write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    
    if (events_ & POLLIN) {
        FD_SET(fd_, &read_fds);
    }
    if (events_ & POLLOUT) {
        FD_SET(fd_, &write_fds);
    }
    
    struct timeval timeout = {0, 0}; // No timeout - immediate check only
    int max_fd = fd_ + 1;
    int ret = select(max_fd, &read_fds, &write_fds, nullptr, &timeout);
    
    if (ret > 0) {
        // I/O is immediately ready
        int result_events = 0;
        if (FD_ISSET(fd_, &read_fds)) result_events |= POLLIN;
        if (FD_ISSET(fd_, &write_fds)) result_events |= POLLOUT;
        
        res_ = result_events;
        return true;
    }
    
    return false; // I/O not immediately ready, need to wait
}

void io_awaitable::start_io_polling()
{
    std::cout << "[DEBUG] Starting background I/O polling for fd=" << fd_ << std::endl;
    
    // CRITICAL FIX: Capture the file descriptor value to prevent corruption
    int captured_fd = fd_;
    unsigned captured_events = events_;
    auto captured_handle = handle_;
    
    std::cout << "[DEBUG] Captured values: fd=" << captured_fd << " events=" << captured_events << std::endl;
    
    // Create a background thread to poll for I/O completion
    std::thread([this, captured_fd, captured_events, captured_handle]() {
        // Use a more direct approach like the working socket test
        fd_set read_fds;
        struct timeval timeout;
        
        std::cout << "[DEBUG] Background thread started for fd=" << captured_fd << std::endl;
        
        // Poll for a reasonable time
        for (int i = 0; i < 1000; ++i) {
            FD_ZERO(&read_fds);
            
            if (captured_events & POLLIN) {
                FD_SET(captured_fd, &read_fds);
            }
            
            // Use shorter timeout for more responsive testing
            timeout.tv_sec = 0;
            timeout.tv_usec = 10000; // 10ms
            
            int max_fd = captured_fd + 1;
            int ret = select(max_fd, &read_fds, nullptr, nullptr, &timeout);
            
            if (i % 100 == 0) {
                std::cout << "[DEBUG] Poll " << i << ": ret=" << ret << " for fd=" << captured_fd << std::endl;
            }
            
            if (ret > 0) {
                // I/O is ready!
                if (FD_ISSET(captured_fd, &read_fds)) {
                    std::cout << "[DEBUG] SUCCESS: Connection detected on fd=" << captured_fd << " after " << i << " polls!" << std::endl;
                    
                    // Store result and resume
                    res_ = POLLIN;
                    std::cout << "[DEBUG] Resuming coroutine..." << std::endl;
                    vthread_scheduler::instance().resume_from_io(captured_handle, POLLIN);
                    return;
                }
            } else if (ret < 0) {
                std::cout << "[DEBUG] select() error: " << strerror(errno) << std::endl;
                res_ = -1;
                vthread_scheduler::instance().resume_from_io(captured_handle, -1);
                return;
            }
            // ret == 0: timeout, continue polling
        }
        
        std::cout << "[DEBUG] Polling timed out after 1000 attempts for fd=" << captured_fd << std::endl;
        res_ = -2; // Timeout
        vthread_scheduler::instance().resume_from_io(captured_handle, -2);
    }).detach();
}



int io_awaitable::await_resume()
{
    std::cout << "[DEBUG] io_awaitable::await_resume called, result=" << res_ << std::endl;
    
    try {
        if (res_ == -1) {
            std::cout << "[DEBUG] await_resume: I/O error occurred, throwing io_error" << std::endl;
            throw std::runtime_error("I/O operation failed");
        } else if (res_ == -3 || res_ == -4) {
            std::cout << "[DEBUG] await_resume: exception in background thread, throwing exception_error" << std::endl;
            throw std::runtime_error("Exception in I/O background thread");
        } else if (res_ == -2) {
            // Timeout is expected behavior for accept() - just return a special value
            std::cout << "[DEBUG] await_resume: timeout occurred (expected), returning -2" << std::endl;
            return -2; // Don't throw for timeout
        }
        
        std::cout << "[DEBUG] await_resume: returning result=" << res_ << std::endl;
        return res_;
        
    } catch (const std::exception& e) {
        std::cout << "[DEBUG] await_resume: EXCEPTION during resume: " << e.what() << std::endl;
        throw; // Re-throw
    }
}
