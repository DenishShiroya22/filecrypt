#pragma once
#include "filecrypt/chunk.hpp"
#include "filecrypt/buffer_pool.hpp"
#include "filecrypt/in_flight_window.hpp"
#include "filecrypt/thread_safe_queue.hpp"
#include <filesystem>
#include <fstream>

namespace filecrypt {
namespace detail { struct RuntimeHooks; }
class ChunkReader {
public:
    explicit ChunkReader(const std::filesystem::path& input);
    std::uintmax_t size() const noexcept { return size_; }
    std::size_t allocations() const noexcept { return allocations_; }
    bool read_next(Chunk& chunk, std::size_t chunk_size, detail::RuntimeHooks* hooks = nullptr);
    void verify_end();
    void produce(std::size_t chunk_size, ThreadSafeQueue<Chunk>& tasks,
                 InFlightWindow& window, detail::RuntimeHooks* hooks = nullptr,
                 BufferPool* buffers = nullptr);
private:
    std::ifstream stream_;
    const std::uintmax_t size_;
    std::uintmax_t remaining_;
    std::size_t next_id_{}, allocations_{};
    bool finished_{false};
};
// Private sibling staging and atomic no-overwrite hard-link publication.
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
                            std::size_t chunk_size, detail::RuntimeHooks* hooks = nullptr,
                            BufferPool* buffers = nullptr);
} // namespace filecrypt
