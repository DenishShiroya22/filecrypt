#include "filecrypt/crypto.hpp"
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace filecrypt {
RollingXor::RollingXor(std::vector<std::uint8_t> key) : key_(std::move(key)) {
    if (key_.empty()) throw std::invalid_argument("Key must not be empty");
}
void RollingXor::transform(Chunk& chunk, std::size_t chunk_size) const {
    const auto id = static_cast<std::uint64_t>(chunk.chunk_id);
    const auto size = static_cast<std::uint64_t>(chunk_size);
    if (!size || id > std::numeric_limits<std::uint64_t>::max() / size)
        throw std::overflow_error("Chunk offset exceeds 64 bits");
    const std::uint64_t offset = id * size;
    std::size_t index = static_cast<std::size_t>(offset % key_.size());
    std::uint8_t roll = static_cast<std::uint8_t>(offset / key_.size());
    // Equivalent to key[position % key_length] + floor(position/key_length)
    // modulo 256. No division/modulo in the hot byte loop.
    for (auto& byte : chunk.data) {
        byte ^= static_cast<std::uint8_t>(key_[index] + roll);
        if (++index == key_.size()) {
            index = 0;
            roll = static_cast<std::uint8_t>(roll + 1U);
        }
    }
}
std::vector<std::uint8_t> read_key(const std::filesystem::path& path) {
    constexpr std::uintmax_t max_key_bytes = 1024U * 1024U;
    if (!std::filesystem::is_regular_file(path))
        throw std::invalid_argument("Key path must be a regular file");
    const auto bytes = std::filesystem::file_size(path);
    if (!bytes || bytes > max_key_bytes)
        throw std::invalid_argument("Key file must contain 1 byte to 1 MiB");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open key file");
    std::vector<std::uint8_t> key(static_cast<std::size_t>(bytes));
    stream.read(reinterpret_cast<char*>(key.data()), static_cast<std::streamsize>(key.size()));
    if (stream.gcount() != static_cast<std::streamsize>(key.size()) || stream.bad())
        throw std::runtime_error("Cannot read complete key file");
    if (stream.peek() != std::char_traits<char>::eof() || stream.bad())
        throw std::runtime_error("Key file changed during read");
    return key;
}
} // namespace filecrypt
