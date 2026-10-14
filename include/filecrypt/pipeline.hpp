#pragma once
#include "filecrypt/crypto.hpp"
#include "filecrypt/file_io.hpp"
#include <cstddef>
#include <filesystem>

namespace filecrypt {
struct Options {
    std::size_t chunk_size{4U * 1024U * 1024U};
    std::size_t workers{}; // Zero selects hardware_concurrency(), falling back to 1.
    std::size_t queue_capacity{8};
    std::size_t max_in_flight{16};
};
struct Stats : WriteStats {
    std::size_t peak_in_flight{}, workers{};
};
Stats process_file(const std::filesystem::path& input,
                   const std::filesystem::path& output,
                   const RollingXor& cipher, Options options = {});
} // namespace filecrypt
