#pragma once
#include "filecrypt/chunk.hpp"
#include <filesystem>
#include <vector>

namespace filecrypt {
// Educational rolling XOR only: no nonce, authentication, or strong secrecy.
// Immutable key state is safe to share between all workers.
class RollingXor {
public:
    explicit RollingXor(std::vector<std::uint8_t> key);
    void transform(Chunk& chunk, std::size_t chunk_size) const;
private:
    const std::vector<std::uint8_t> key_;
};
std::vector<std::uint8_t> read_key(const std::filesystem::path& path);
} // namespace filecrypt
