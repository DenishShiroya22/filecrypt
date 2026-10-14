#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace filecrypt {
// shutdown() closes admission and drains existing work; shutdown(true)
// cancels and discards it. Both wake every blocked producer and consumer.
template <class T>
class ThreadSafeQueue {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "Queue elements must have a nonthrowing move constructor");
public:
    explicit ThreadSafeQueue(std::size_t capacity) : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("Queue capacity must be positive");
    }
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    // The caller retains ownership if closed. No payload is copied.
    bool push(T&& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_) return false;
        queue_.emplace_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return std::nullopt;
        std::optional<T> value(std::in_place, std::move(queue_.front()));
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }
    void shutdown(bool discard = false) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            if (discard) queue_.clear();
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }
private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable not_full_, not_empty_;
    std::deque<T> queue_;
    bool closed_{false};
};
} // namespace filecrypt
