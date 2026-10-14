#pragma once
#include <cstddef>
#include <functional>
#include <filesystem>
#include "filecrypt/thread_safe_queue.hpp"
#include "filecrypt/pipeline.hpp"

namespace filecrypt::detail {
// Fault injection at real execution boundaries, used only by tests.
// Chunk callbacks can run on different threads: captured shared state must be
// synchronized. Queue observers run under the queue mutex, must not throw,
// block, or re-enter a queue. The caller must keep hooks alive until return.
struct RuntimeHooks {
    std::function<void(std::size_t)> before_allocate, before_read;
    std::function<void(std::size_t)> before_transform, before_write, before_launch;
    ThreadSafeQueue<Chunk>::OnWait task_wait, result_wait;
};
Stats process_file_with_hooks(const std::filesystem::path& input,
    const std::filesystem::path& output, const RollingXor& cipher,
    Options options, RuntimeHooks& hooks);
} // namespace filecrypt::detail
