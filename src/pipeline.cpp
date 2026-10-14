#include "filecrypt/pipeline.hpp"
#include "filecrypt/thread_pool.hpp"
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace filecrypt {
Stats process_file(const std::filesystem::path& input, const std::filesystem::path& output,
                   const RollingXor& cipher, Options options) {
    if (!options.workers) {
        options.workers = std::thread::hardware_concurrency();
        if (!options.workers) options.workers = 1;
    }
    if (!options.chunk_size || options.chunk_size >
            static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) ||
        !options.queue_capacity || !options.max_in_flight)
        throw std::invalid_argument("Chunk size and capacities must be positive and representable");
    if (options.chunk_size > std::numeric_limits<std::size_t>::max() / options.max_in_flight)
        throw std::overflow_error("Memory budget exceeds address space");
    ChunkReader reader(input);
    const auto chunk_count = reader.size() == 0 ? 1 :
        reader.size() / options.chunk_size + (reader.size() % options.chunk_size != 0 ? 1U : 0U);
    if (reader.size() > std::numeric_limits<std::uint64_t>::max() ||
        chunk_count >= std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("Input exceeds supported offset or chunk range");
    OutputTransaction transaction(output);
    ThreadSafeQueue<Chunk> tasks(options.queue_capacity), results(options.queue_capacity);
    InFlightWindow window(options.max_in_flight);
    std::mutex error_mutex, completion_mutex;
    std::exception_ptr first_error;
    std::size_t active_workers = options.workers;
    auto fail = [&](std::exception_ptr error) noexcept {
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!first_error) first_error = error;
        }
        window.cancel();
        tasks.shutdown(true);
        results.shutdown(true);
    };
    auto worker = [&] {
        try {
            while (auto chunk = tasks.pop()) {
                cipher.transform(*chunk, options.chunk_size);
                if (!results.push(std::move(*chunk))) break;
            }
        } catch (...) { fail(std::current_exception()); }
        std::lock_guard<std::mutex> lock(completion_mutex);
        if (--active_workers == 0) results.shutdown();
    };
    // Pointers allow a single outer catch to cancel BEFORE pool destructors join.
    // This also handles allocation/argument-copy failures before constructors run.
    std::unique_ptr<ThreadPool> workers, producer;
    Stats stats;
    try {
        workers = std::make_unique<ThreadPool>(options.workers, worker, fail);
        producer = std::make_unique<ThreadPool>(1, [&] {
            reader.produce(options.chunk_size, tasks, window);
        }, fail);
        static_cast<WriteStats&>(stats) = write_sequential(
            transaction.stream(), results, window, reader.size(), options.chunk_size);
    } catch (...) { fail(std::current_exception()); }
    if (producer) producer->join();
    if (workers) workers->join();
    // Every participant is now joined; error inspection and publication are safe.
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (first_error) std::rethrow_exception(first_error);
    }
    stats.peak_in_flight = window.peak();
    stats.workers = options.workers;
    transaction.commit();
    return stats;
}
} // namespace filecrypt
