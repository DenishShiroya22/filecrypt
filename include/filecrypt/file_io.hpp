#pragma once
#include "filecrypt/chunk.hpp"
#include "filecrypt/in_flight_window.hpp"
#include "filecrypt/thread_safe_queue.hpp"
#include <filesystem>
#include <fstream>

namespace filecrypt {
class ChunkReader {
public:
    explicit ChunkReader(const std::filesystem::path& input);
    std::uintmax_t size() const noexcept { return size_; }
    void produce(std::size_t chunk_size, ThreadSafeQueue<Chunk>& tasks, InFlightWindow& window);
private:
    std::ifstream stream_;
    const std::uintmax_t size_;
};
// Writes privately beside the destination. Publication uses an atomic hard-link
// creation so an existing destination (including a raced creation) is never lost.
class OutputTransaction {
public:
    explicit OutputTransaction(const std::filesystem::path& destination);
    ~OutputTransaction();
    OutputTransaction(const OutputTransaction&) = delete;
    OutputTransaction& operator=(const OutputTransaction&) = delete;
    std::ostream& stream() noexcept { return stream_; }
    void commit();
private:
    std::filesystem::path destination_, directory_, staged_;
    std::ofstream stream_;
};
struct WriteStats {
    std::uintmax_t bytes{};
    std::size_t chunks{}, peak_reordered{};
};
WriteStats write_sequential(std::ostream& output, ThreadSafeQueue<Chunk>& results,
                            InFlightWindow& window, std::uintmax_t input_size,
                            std::size_t chunk_size);
} // namespace filecrypt
