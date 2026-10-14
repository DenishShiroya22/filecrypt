#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace filecrypt {
// Credits govern admission; this pool governs storage reuse. It never waits.
// At most limit vectors are created. Returned storage is kept at its current
// size so the next full read avoids allocation AND redundant zero filling.
class BufferPool {
public:
    explicit BufferPool(std::size_t limit) : limit_(limit) {
        if (!limit) throw std::invalid_argument("Buffer pool limit must be positive");
        free_.reserve(limit);
    }
    std::vector<std::uint8_t> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!free_.empty()) {
            auto data = std::move(free_.back());
            free_.pop_back();
            return data;
        }
        if (created_ == limit_)
            throw std::logic_error("Buffer admission invariant violated");
        ++created_;
        return {};
    }
    void give(std::vector<std::uint8_t>&& data) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        // reserve(limit) plus exactly-once return makes this non-allocating.
        free_.emplace_back(std::move(data));
    }
private:
    const std::size_t limit_;
    std::mutex mutex_;
    std::size_t created_{};
    std::vector<std::vector<std::uint8_t>> free_;
};
} // namespace filecrypt
