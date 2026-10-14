#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace filecrypt {
enum class QueueWait { push, pop };
struct QueueStats { std::size_t push_waits{}, pop_waits{}, peak_size{}; };
// Queue lifetime must outlast every caller. shutdown() drains; shutdown(true)
// discards. Both wake blocked producers/consumers. No payload is copied.
template <class T>
class ThreadSafeQueue {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "Queue elements must have a nonthrowing move constructor");
public:
    using OnWait = std::function<void(QueueWait)>;
    explicit ThreadSafeQueue(std::size_t capacity, OnWait observer = {})
        : capacity_(capacity), observer_(std::move(observer)) {
        if (!capacity) throw std::invalid_argument("Queue capacity must be positive");
    }
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    bool push(T&& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!closed_ && queue_.size() == capacity_) {
            ++stats_.push_waits;
            if (observer_) observer_(QueueWait::push);
        }
        not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_) return false;
        queue_.emplace_back(std::move(value));
        if (queue_.size() > stats_.peak_size) stats_.peak_size = queue_.size();
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!closed_ && queue_.empty()) {
            ++stats_.pop_waits;
            if (observer_) observer_(QueueWait::pop);
        }
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
    QueueStats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }
private:
    const std::size_t capacity_;
    const OnWait observer_;
    mutable std::mutex mutex_;
    std::condition_variable not_full_, not_empty_;
    std::deque<T> queue_;
    QueueStats stats_;
    bool closed_{false};
};
} // namespace filecrypt
