#pragma once
#include "filecrypt/crypto.hpp"
#include "filecrypt/file_io.hpp"
#include <cstddef>
#include <filesystem>

namespace filecrypt {
enum class Engine { pipeline, sequential };
struct Options {
    std::size_t chunk_size{4U * 1024U * 1024U};
    std::size_t workers{}; // Zero selects hardware concurrency, falling back to 1.
    std::size_t queue_capacity{8};
    std::size_t max_in_flight{16};
    Engine engine{Engine::pipeline};
};
struct Stats : WriteStats {
    std::size_t peak_in_flight{}, workers{}, buffer_allocations{};
    QueueStats task_queue, result_queue;
};
Stats process_file(const std::filesystem::path& input,
                   const std::filesystem::path& output,
                   const RollingXor& cipher, Options options = {});
} // namespace filecrypt
