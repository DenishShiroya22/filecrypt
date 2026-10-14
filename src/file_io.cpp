#include "filecrypt/file_io.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace filecrypt {
namespace fs = std::filesystem;
namespace {
std::uintmax_t regular_file_size(const fs::path& path) {
    if (!fs::is_regular_file(path))
        throw std::invalid_argument("Input must be a regular file");
    return fs::file_size(path);
}
}
ChunkReader::ChunkReader(const fs::path& input)
    : stream_(input, std::ios::binary), size_(regular_file_size(input)) {
    if (!stream_) throw std::runtime_error("Cannot open input file");
}
void ChunkReader::produce(std::size_t chunk_size, ThreadSafeQueue<Chunk>& tasks,
                          InFlightWindow& window) {
    std::uintmax_t remaining = size_;
    std::size_t id = 0;
    // Empty files still carry a single terminal chunk, keeping EOF explicit.
    do {
        if (!window.acquire()) return;
        const auto bytes = static_cast<std::size_t>(
            std::min<std::uintmax_t>(remaining, chunk_size));
        Chunk chunk{id, std::vector<std::uint8_t>(bytes), remaining <= chunk_size};
        if (bytes) {
            stream_.read(reinterpret_cast<char*>(chunk.data.data()),
                         static_cast<std::streamsize>(bytes));
            if (stream_.gcount() != static_cast<std::streamsize>(bytes) || stream_.bad())
                throw std::runtime_error("Read failed or input shrank during processing");
        }
        remaining -= bytes;
        if (!tasks.push(std::move(chunk))) return;
        if (remaining) ++id;
    } while (remaining);
    if (stream_.peek() != std::char_traits<char>::eof() || stream_.bad())
        throw std::runtime_error("Read failed or input grew during processing");
    tasks.shutdown();
}
OutputTransaction::OutputTransaction(const fs::path& destination)
    : destination_(fs::absolute(destination).lexically_normal()) {
    if (destination_.filename().empty())
        throw std::invalid_argument("Output must name a file");
    // symlink_status sees even a dangling symlink; publication repeats this
    // protection atomically, rather than relying on a racy exists() check.
    const auto status = fs::symlink_status(destination_);
    if (status.type() != fs::file_type::not_found)
        throw std::runtime_error("Output already exists; refusing to overwrite");
    std::random_device random;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto name = ".filecrypt-" + std::to_string(random()) + "-" +
                          std::to_string(random()) + "-" + std::to_string(attempt);
        const auto candidate = destination_.parent_path() / name;
        if (fs::create_directory(candidate)) { directory_ = candidate; break; }
    }
    if (directory_.empty()) throw std::runtime_error("Cannot reserve staging directory");
    try {
        fs::permissions(directory_, fs::perms::owner_all, fs::perm_options::replace);
        staged_ = directory_ / "data";
        stream_.open(staged_, std::ios::binary | std::ios::trunc);
        if (!stream_) throw std::runtime_error("Cannot open staged output");
        fs::permissions(staged_, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace);
    } catch (...) {
        stream_.close();
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
        throw;
    }
}
OutputTransaction::~OutputTransaction() {
    if (stream_.is_open()) stream_.close();
    std::error_code ignored;
    if (!directory_.empty()) fs::remove_all(directory_, ignored);
}
void OutputTransaction::commit() {
    stream_.flush();
    if (!stream_) throw std::runtime_error("Failed to flush output");
    stream_.close();
    if (stream_.fail()) throw std::runtime_error("Failed to close output");
    // Source and destination are on the same filesystem. Unsupported hard links
    // fail safely; a rename fallback would reintroduce overwrite races.
    fs::create_hard_link(staged_, destination_);
}
WriteStats write_sequential(std::ostream& output, ThreadSafeQueue<Chunk>& results,
                            InFlightWindow& window, std::uintmax_t input_size,
                            std::size_t chunk_size) {
    const std::uintmax_t count64 = input_size == 0 ? 1 :
        input_size / chunk_size + (input_size % chunk_size != 0 ? 1U : 0U);
    if (count64 > std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("Too many chunks for this platform");
    const auto count = static_cast<std::size_t>(count64);
    std::unordered_map<std::size_t, Chunk> pending;
    WriteStats stats;
    std::size_t expected = 0;
    while (auto chunk = results.pop()) {
        const auto id = chunk->chunk_id;
        if (id < expected || id >= count)
            throw std::runtime_error("Invalid or repeated result ID");
        const std::uintmax_t offset = static_cast<std::uintmax_t>(id) * chunk_size;
        const auto bytes = static_cast<std::size_t>(
            std::min<std::uintmax_t>(input_size - offset, chunk_size));
        if (chunk->data.size() != bytes || chunk->is_last != (id == count - 1))
            throw std::runtime_error("Invalid result size or terminal flag");
        if (!pending.emplace(id, std::move(*chunk)).second)
            throw std::runtime_error("Repeated pending result ID");
        stats.peak_reordered = std::max(stats.peak_reordered, pending.size());
        for (;;) {
            auto it = pending.find(expected);
            if (it == pending.end()) break;
            if (!it->second.data.empty()) {
                output.write(reinterpret_cast<const char*>(it->second.data.data()),
                             static_cast<std::streamsize>(it->second.data.size()));
                if (!output) throw std::runtime_error("Output write failed");
            }
            stats.bytes += it->second.data.size();
            ++stats.chunks;
            ++expected;
            pending.erase(it); // Free payload BEFORE making another credit available.
            window.release();
        }
    }
    if (expected != count || !pending.empty() || stats.bytes != input_size)
        throw std::runtime_error("Pipeline stopped before all chunks were written");
    return stats;
}
} // namespace filecrypt
