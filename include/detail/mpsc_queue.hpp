#ifndef mpsc_queue_hpp
#define mpsc_queue_hpp

#include <atomic>
#include <cstddef>
#include <utility>

namespace swiftnet::detail
{

    /* Multiple-producer / single-consumer intrusive queue.
     * Producer: lock-free push()
     * Consumer: pop() - single thread.
     */
    template <typename T>
    class mpsc_queue
    {
        struct node
        {
            T value;
            std::atomic<node *> next{nullptr};
            explicit node(T v) : value(std::move(v)) {}
        };

        std::atomic<node *> tail_;
        node *head_; // consumer-only

    public:
        mpsc_queue()
        {
            auto *dummy = new node(T{});
            head_ = dummy;
            tail_.store(dummy, std::memory_order_relaxed);
        }

        ~mpsc_queue()
        {
            while (head_)
            {
                node *nxt = head_->next.load(std::memory_order_relaxed);
                delete head_;
                head_ = nxt;
            }
        }

        void push(T v) noexcept
        {
            auto *n = new node(std::move(v));
            n->next.store(nullptr, std::memory_order_relaxed);
            node *prev = tail_.exchange(n, std::memory_order_acq_rel);
            prev->next.store(n, std::memory_order_release);
        }

        bool pop(T &out) noexcept
        {
            node *next = head_->next.load(std::memory_order_acquire);
            if (!next)
                return false;
            out = std::move(next->value);
            delete head_;
            head_ = next;
            return true;
        }

        bool empty() const noexcept
        {
            return head_->next.load(std::memory_order_acquire) == nullptr;
        }
    };

}

#endif
