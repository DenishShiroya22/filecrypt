#include "filecrypt/pipeline.hpp"
#include "filecrypt/detail/runtime_hooks.hpp"
#include "filecrypt/thread_pool.hpp"
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace filecrypt {
namespace {
void validate(Options& options, std::uintmax_t input_size) {
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
    const auto count = input_size == 0 ? 1 :
        input_size / options.chunk_size + (input_size % options.chunk_size != 0 ? 1U : 0U);
    if (input_size > std::numeric_limits<std::uint64_t>::max() ||
        count >= std::numeric_limits<std::size_t>::max())
        throw std::overflow_error("Input exceeds supported offset or chunk range");
}
Stats run_sequential(ChunkReader& reader, std::ostream& output,
                     const RollingXor& cipher, const Options& options,
                     detail::RuntimeHooks* hooks) {
    Stats stats;
    Chunk chunk; // One reusable vector; this path starts no threads or queues.
    while (reader.read_next(chunk, options.chunk_size, hooks)) {
        if (hooks && hooks->before_transform) hooks->before_transform(chunk.chunk_id);
        cipher.transform(chunk, options.chunk_size);
        if (hooks && hooks->before_write) hooks->before_write(chunk.chunk_id);
        if (!chunk.data.empty()) {
            output.write(reinterpret_cast<const char*>(chunk.data.data()),
                         static_cast<std::streamsize>(chunk.data.size()));
            if (!output) throw std::runtime_error("Output write failed");
        }
        stats.bytes += chunk.data.size();
        ++stats.chunks;
    }
    reader.verify_end();
    stats.peak_in_flight = 1;
    stats.buffer_allocations = reader.allocations();
    return stats;
}
Stats run_pipeline(ChunkReader& reader, std::ostream& output,
                   const RollingXor& cipher, const Options& options,
                   detail::RuntimeHooks* hooks) {
    ThreadSafeQueue<Chunk> tasks(options.queue_capacity, hooks ? hooks->task_wait : nullptr);
    ThreadSafeQueue<Chunk> results(options.queue_capacity, hooks ? hooks->result_wait : nullptr);
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
                if (hooks && hooks->before_transform) hooks->before_transform(chunk->chunk_id);
                cipher.transform(*chunk, options.chunk_size);
                if (!results.push(std::move(*chunk))) break;
            }
        } catch (...) { fail(std::current_exception()); }
        std::lock_guard<std::mutex> lock(completion_mutex);
        if (--active_workers == 0) results.shutdown();
    };
    std::size_t launch_index = 0;
    auto before_launch = [&](std::size_t) {
        if (hooks && hooks->before_launch) hooks->before_launch(launch_index);
        ++launch_index;
    };
    // Cancel BEFORE joining on every startup and runtime failure.
    std::unique_ptr<ThreadPool> workers, producer;
    Stats stats;
    try {
        workers = std::make_unique<ThreadPool>(options.workers, worker, fail, before_launch);
        producer = std::make_unique<ThreadPool>(1, [&] {
            reader.produce(options.chunk_size, tasks, window, hooks);
        }, fail, before_launch);
        static_cast<WriteStats&>(stats) = write_sequential(
            output, results, window, reader.size(), options.chunk_size, hooks);
    } catch (...) { fail(std::current_exception()); }
    if (producer) producer->join();
    if (workers) workers->join();
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (first_error) std::rethrow_exception(first_error);
    }
    stats.peak_in_flight = window.peak();
    stats.workers = options.workers;
    stats.buffer_allocations = reader.allocations();
    stats.task_queue = tasks.stats();
    stats.result_queue = results.stats();
    return stats;
}
Stats process_impl(const std::filesystem::path& input, const std::filesystem::path& output,
                   const RollingXor& cipher, Options options, detail::RuntimeHooks* hooks) {
    ChunkReader reader(input);
    validate(options, reader.size());
    OutputTransaction transaction(output);
    Stats stats;
    if (options.engine == Engine::sequential)
        stats = run_sequential(reader, transaction.stream(), cipher, options, hooks);
    else if (options.engine == Engine::pipeline)
        stats = run_pipeline(reader, transaction.stream(), cipher, options, hooks);
    else throw std::invalid_argument("Invalid processing engine");
    transaction.commit();
    return stats;
}
}
Stats process_file(const std::filesystem::path& input, const std::filesystem::path& output,
                   const RollingXor& cipher, Options options) {
    return process_impl(input, output, cipher, options, nullptr);
}
namespace detail {
Stats process_file_with_hooks(const std::filesystem::path& input,
    const std::filesystem::path& output, const RollingXor& cipher,
    Options options, RuntimeHooks& hooks) {
    return process_impl(input, output, cipher, options, &hooks);
}
}
} // namespace filecrypt
