#ifndef DUSK_NET_QUEUE_H
#define DUSK_NET_QUEUE_H

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace dusk::net {

// Single-mutex bounded queue used to hand packet effects between the network
// worker thread and the sim thread. SPSC at both ends today; the std::mutex
// keeps the implementation obvious and trivially safe at our 30 Hz traffic.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    // Returns false (and drops `value`) if the queue is at capacity. Use for
    // reliable channels where backpressure must surface as an error.
    bool push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deque_.size() >= capacity_) {
            return false;
        }
        deque_.push_back(std::move(value));
        return true;
    }

    // If the queue is full, drops the oldest element to make room. Use for
    // unreliable-sequenced channels (pose snapshots) where the newest value
    // is the only one worth keeping.
    void push_or_drop_oldest(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deque_.size() >= capacity_) {
            deque_.pop_front();
        }
        deque_.push_back(std::move(value));
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deque_.empty()) {
            return std::nullopt;
        }
        T v = std::move(deque_.front());
        deque_.pop_front();
        return v;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deque_.size();
    }

    std::size_t capacity() const { return capacity_; }

private:
    mutable std::mutex mutex_;
    std::deque<T> deque_;
    std::size_t capacity_;
};

}  // namespace dusk::net

#endif
