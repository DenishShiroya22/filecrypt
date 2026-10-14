#pragma once
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>

namespace filecrypt {
// A credit spans reader allocation, both queues, a worker, and the reorder
// buffer. Only an ordered write returns it. This bounds the WHOLE pipeline
// and lets the writer continue draining results even when an early chunk lags.
class InFlightWindow {
public:
    explicit InFlightWindow(std::size_t capacity) : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("In-flight limit must be positive");
    }
    bool acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [this] { return cancelled_ || outstanding_ < capacity_; });
        if (cancelled_) return false;
        ++outstanding_;
        if (outstanding_ > peak_) peak_ = outstanding_;
        return true;
    }
    void release() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --outstanding_; // Exactly once, after each successfully written chunk.
        }
        available_.notify_one();
    }
    void cancel() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
        }
        available_.notify_all();
    }
    std::size_t peak() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_;
    }
private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::size_t outstanding_{}, peak_{};
    bool cancelled_{false};
};
} // namespace filecrypt
