#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace filecrypt {
struct Chunk {
    std::size_t chunk_id{};
    std::vector<std::uint8_t> data;
    bool is_last{};
};
} // namespace filecrypt
