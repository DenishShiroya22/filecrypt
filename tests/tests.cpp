#include "filecrypt/pipeline.hpp"
#include "filecrypt/thread_pool.hpp"
#include "filecrypt/detail/runtime_hooks.hpp"
#include <condition_variable>
#include <system_error>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace filecrypt;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Function>
void throws(Function function) {
    bool caught = false;
    try { function(); } catch (const std::exception&) { caught = true; }
    require(caught, "Expected an exception");
}
struct TempDirectory {
    fs::path path;
    TempDirectory() {
        std::random_device random;
        for (int i = 0; i < 100; ++i) {
            const auto candidate = fs::temp_directory_path() /
                ("filecrypt-test-" + std::to_string(random()) + "-" + std::to_string(i));
            if (fs::create_directory(candidate)) { path = candidate; return; }
        }
        throw std::runtime_error("Cannot create test directory");
    }
    ~TempDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
std::vector<std::uint8_t> random_bytes(std::size_t size) {
    std::mt19937 random(123456);
    std::vector<std::uint8_t> result(size);
    for (auto& byte : result) byte = static_cast<std::uint8_t>(random());
    return result;
}
void save(const fs::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream output(path, std::ios::binary);
    if (!data.empty())
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    output.close(); require(!output.fail(), "Fixture write failed");
}
std::vector<std::uint8_t> read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void queue_shutdown() {
    throws([] { ThreadSafeQueue<int> invalid(0); });
    ThreadSafeQueue<std::unique_ptr<int>> queue(1);
    auto first = std::make_unique<int>(7);
    require(queue.push(std::move(first)), "Initial push failed");
    require(!first, "Queue must move ownership");
    std::promise<void> started;
    auto blocked = std::async(std::launch::async, [&] {
        started.set_value();
        auto value = std::make_unique<int>(8);
        const bool accepted = queue.push(std::move(value));
        return !accepted && value && *value == 8;
    });
    started.get_future().wait();
    const bool waited = blocked.wait_for(30ms) == std::future_status::timeout;
    queue.shutdown(); // Always unblock before assertions/future destruction.
    require(blocked.get(), "Shutdown must reject blocked push without moving it");
    require(waited, "Bounded push should wait");
    auto item = queue.pop();
    require(item && **item == 7, "Graceful shutdown must drain");
    require(!queue.pop(), "Closed drained queue must return EOF");
    ThreadSafeQueue<int> empty(1);
    auto consumer = std::async(std::launch::async, [&] { return empty.pop(); });
    empty.shutdown(true);
    require(!consumer.get(), "Shutdown must unblock consumer");
    ThreadSafeQueue<int> discard(1);
    discard.push(1); discard.shutdown(true);
    require(!discard.pop(), "Cancellation must discard queued payloads");
}
void queue_contention() {
    ThreadSafeQueue<int> queue(1);
    std::atomic<int> received{0};
    std::vector<std::thread> consumers, producers;
    for (int i = 0; i < 4; ++i)
        consumers.emplace_back([&] { while (auto item = queue.pop()) received += *item; });
    for (int i = 0; i < 4; ++i)
        producers.emplace_back([&] { for (int n = 0; n < 1000; ++n) queue.push(1); });
    for (auto& thread : producers) thread.join();
    queue.shutdown();
    for (auto& thread : consumers) thread.join();
    require(received == 4000, "Contended queue lost or duplicated items");
}
void window_cancel() {
    InFlightWindow window(1);
    require(window.acquire(), "Credit acquire failed");
    auto waiter = std::async(std::launch::async, [&] { return window.acquire(); });
    const bool waited = waiter.wait_for(30ms) == std::future_status::timeout;
    window.cancel();
    require(!waiter.get() && waited && window.peak() == 1, "Window bound/cancellation failed");
}
void cipher_offsets() {
    for (const std::size_t key_size : {1U, 5U, 263U}) {
        const auto key = random_bytes(key_size);
        RollingXor cipher(key);
        const auto original = random_bytes(25001);
        auto reference = original;
        for (std::size_t i = 0; i < reference.size(); ++i)
            reference[i] ^= static_cast<std::uint8_t>(key[i % key.size()] +
                                                    (i / key.size()) % 256U);
        for (const std::size_t block_size : {1U, 17U, 4096U, 8191U}) {
            std::vector<Chunk> chunks;
            for (std::size_t start = 0, id = 0; start < original.size(); start += block_size, ++id) {
                const auto end = std::min(start + block_size, original.size());
                chunks.push_back({id, {original.begin() + static_cast<std::ptrdiff_t>(start),
                                      original.begin() + static_cast<std::ptrdiff_t>(end)},
                                  end == original.size()});
            }
            // Reverse completion explicitly proves independence from processing order.
            for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) cipher.transform(*it, block_size);
            std::vector<std::uint8_t> combined;
            for (const auto& chunk : chunks) combined.insert(combined.end(), chunk.data.begin(), chunk.data.end());
            require(combined == reference, "Cipher differs across partitions");
            for (auto& chunk : chunks) cipher.transform(chunk, block_size);
            combined.clear();
            for (const auto& chunk : chunks) combined.insert(combined.end(), chunk.data.begin(), chunk.data.end());
            require(combined == original, "XOR round trip failed");
        }
        if constexpr (std::numeric_limits<std::size_t>::digits >= 64) {
            Chunk overflow{std::numeric_limits<std::size_t>::max(), {}, true};
            throws([&] { cipher.transform(overflow, 2); });
        }
    }
    throws([] { RollingXor invalid({}); });
}
struct FailingBuffer : std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
};
void ordered_writer() {
    ThreadSafeQueue<Chunk> results(3);
    InFlightWindow window(3);
    for (int i = 0; i < 3; ++i) window.acquire();
    results.push(Chunk{2, {'e'}, true});
    results.push(Chunk{1, {'c', 'd'}, false});
    results.push(Chunk{0, {'a', 'b'}, false});
    results.shutdown();
    std::ostringstream output;
    const auto stats = write_sequential(output, results, window, 5, 2);
    require(output.str() == "abcde" && stats.peak_reordered == 3 && stats.chunks == 3,
            "Writer must reorder exact bytes");
    ThreadSafeQueue<Chunk> missing(1);
    missing.shutdown();
    throws([&] { write_sequential(output, missing, window, 1, 1); });
    ThreadSafeQueue<Chunk> malformed(1);
    malformed.push(Chunk{0, {'a'}, false}); malformed.shutdown();
    throws([&] { write_sequential(output, malformed, window, 1, 1); });
    ThreadSafeQueue<Chunk> failing(1);
    failing.push(Chunk{0, {'a'}, true}); failing.shutdown();
    FailingBuffer buffer; std::ostream bad(&buffer);
    throws([&] { write_sequential(bad, failing, window, 1, 1); });
}
void thread_pool_errors() {
    std::atomic<int> errors{0};
    ThreadPool pool(4, [] { throw std::runtime_error("Injected worker error"); },
                    [&](std::exception_ptr) noexcept { ++errors; });
    pool.join();
    require(errors == 4, "Worker exceptions must reach callback");
}
void pipeline_roundtrip() {
    TempDirectory temp;
    RollingXor cipher({0, 7, 255, 16, 42});
    std::size_t case_id = 0;
    for (const std::size_t bytes : {0U, 1U, 4095U, 4096U, 4097U, 1024U * 1024U + 17U}) {
        const auto input = temp.path / ("input-" + std::to_string(case_id));
        const auto encrypted = temp.path / ("enc-" + std::to_string(case_id));
        const auto decrypted = temp.path / ("dec-" + std::to_string(case_id++));
        const auto original = random_bytes(bytes);
        save(input, original);
        const auto stats = process_file(input, encrypted, cipher, Options{4096, 8, 1, 3});
        require(stats.bytes == bytes && stats.peak_in_flight <= 3 && stats.peak_reordered <= 3,
                "Pipeline violated integrity/memory limits");
        process_file(encrypted, decrypted, cipher, Options{8191, 2, 2, 1});
        require(read(decrypted) == original, "Different-partition round trip failed");
        throws([&] { process_file(input, encrypted, cipher); });
        throws([&] { process_file(input, input, cipher); });
        require(read(input) == original, "Failure must preserve source");
    }
    const auto absent = temp.path / "absent";
    throws([&] { process_file(absent, temp.path / "never-created", cipher); });
    require(!fs::exists(temp.path / "never-created"), "Missing input must not create output");
    throws([&] { process_file(temp.path, temp.path / "directory-output", cipher); });
    save(temp.path / "empty-key", {});
    throws([&] { read_key(temp.path / "empty-key"); });
    save(temp.path / "key", {0, 1, 255});
    require(read_key(temp.path / "key") == std::vector<std::uint8_t>({0, 1, 255}),
            "Binary key read failed");
}
void output_transaction() {
    TempDirectory temp;
    const auto output = temp.path / "output";
    { OutputTransaction transaction(output); transaction.stream() << "partial"; }
    require(!fs::exists(output) && fs::is_empty(temp.path), "Rollback leaked staged output");
    {
        OutputTransaction transaction(output);
        transaction.stream() << "candidate";
        save(output, {'o', 'l', 'd'}); // Race: destination appears AFTER admission.
        throws([&] { transaction.commit(); });
    }
    require(read(output) == std::vector<std::uint8_t>({'o', 'l', 'd'}),
            "Publication must preserve raced destination");
    require(std::distance(fs::directory_iterator(temp.path), fs::directory_iterator{}) == 1,
            "Failed publication leaked staging directory");
}
void sequential_equivalence() {
    TempDirectory temp;
    const auto input = temp.path / "input";
    const auto data = random_bytes(1024U * 1024U + 19U);
    save(input, data);
    RollingXor cipher({0, 1, 255, 32, 9});
    Options serial{65537, 8, 1, 3, Engine::sequential};
    const auto stats = process_file(input, temp.path / "serial", cipher, serial);
    require(stats.workers == 0 && stats.buffer_allocations == 1 &&
            stats.task_queue.push_waits == 0, "Sequential engine started pipeline resources");
    process_file(input, temp.path / "parallel", cipher, Options{4096, 4, 1, 3});
    require(read(temp.path / "serial") == read(temp.path / "parallel"), "Engine output differs");
    process_file(temp.path / "serial", temp.path / "recovered", cipher,
                 Options{8191, 1, 1, 1, Engine::sequential});
    require(read(temp.path / "recovered") == data, "Sequential round trip failed");
}
void require_rollback(const TempDirectory& temp, const fs::path& output) {
    require(!fs::exists(output), "Failed pipeline published output");
    for (const auto& item : fs::directory_iterator(temp.path))
        require(item.path().filename().string().find(".filecrypt-") != 0,
                "Failed pipeline left staging data");
}
void launch_failures() {
    TempDirectory temp;
    save(temp.path / "input", random_bytes(65536));
    RollingXor cipher({1,2,3});
    for (const std::size_t fail_at : {1U, 3U}) {
        detail::RuntimeHooks hooks;
        std::size_t attempts = 0;
        hooks.before_launch = [&](std::size_t index) {
            ++attempts;
            if (index == fail_at)
                throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                        "Injected thread launch failure");
        };
        bool caught = false;
        try { detail::process_file_with_hooks(temp.path/"input", temp.path/"output",
                                             cipher, Options{4096,3,1,4}, hooks); }
        catch (const std::system_error&) { caught = true; }
        require(caught && attempts == fail_at+1, "Launch failure did not propagate");
        require_rollback(temp, temp.path/"output");
    }
}
void allocation_failure() {
    TempDirectory temp;
    const auto data = random_bytes(65536);
    save(temp.path/"input",data);
    std::mutex mutex;
    std::condition_variable changed;
    bool failed = false;
    detail::RuntimeHooks hooks;
    hooks.before_transform = [&](std::size_t id) {
        if (id != 0) return;
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, 5s, [&]{ return failed; }))
            throw std::runtime_error("Allocation failure gate timed out");
    };
    hooks.before_allocate = [&](std::size_t id) {
        if (id != 2) return;
        { std::lock_guard<std::mutex> lock(mutex); failed = true; }
        changed.notify_all();
        throw std::bad_alloc();
    };
    bool caught = false;
    try { detail::process_file_with_hooks(temp.path/"input", temp.path/"output",
            RollingXor({1,2,3}), Options{1024,2,1,8}, hooks); }
    catch (const std::bad_alloc&) { caught = true; }
    require(caught, "Allocation error was replaced or lost");
    require_rollback(temp,temp.path/"output");
    require(read(temp.path/"input") == data, "Allocation failure modified input");
}
void input_change_failures() {
    for (const bool grow : {false,true}) {
        TempDirectory temp;
        const auto input = temp.path/"input";
        save(input,random_bytes(65536));
        detail::RuntimeHooks hooks;
        hooks.before_read = [&](std::size_t id) {
            if (id != 1) return;
            if (grow) { std::ofstream append(input,std::ios::binary|std::ios::app); append.put('x'); }
            else fs::resize_file(input,0);
        };
        bool caught = false;
        try { detail::process_file_with_hooks(input,temp.path/"output",
                RollingXor({1,2,3}),Options{1024,2,1,4},hooks); }
        catch (const std::runtime_error& error) {
            caught = std::string(error.what()).find(grow ? "grew" : "shrank") != std::string::npos;
        }
        require(caught, "Actual truncated/grown input was not rejected");
        require_rollback(temp,temp.path/"output");
    }
}
void writer_failure() {
    TempDirectory temp;
    const auto data = random_bytes(65536);
    save(temp.path/"input",data);
    detail::RuntimeHooks hooks;
    hooks.before_write = [](std::size_t id) {
        if (id == 2) throw std::runtime_error("Injected write boundary failure");
    };
    bool caught = false;
    try { detail::process_file_with_hooks(temp.path/"input",temp.path/"output",
            RollingXor({1,2,3}), Options{1024,4,1,8}, hooks); }
    catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "Injected write boundary failure";
    }
    require(caught, "Writer failure did not retain original error");
    require_rollback(temp,temp.path/"output");
    require(read(temp.path/"input") == data, "Write failure modified input");
}
void saturated_cancellation() {
    TempDirectory temp;
    const auto data = random_bytes(128U*1024U);
    save(temp.path/"input",data);
    std::mutex mutex;
    std::condition_variable changed;
    bool writer_blocked=false, task_blocked=false, result_blocked=false;
    detail::RuntimeHooks hooks;
    hooks.task_wait = [&](QueueWait wait) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (wait == QueueWait::push && writer_blocked) task_blocked = true;
        changed.notify_all();
    };
    hooks.result_wait = [&](QueueWait wait) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (wait == QueueWait::push && writer_blocked) result_blocked = true;
        changed.notify_all();
    };
    hooks.before_transform = [&](std::size_t id) {
        if (!id) return;
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock,5s,[&]{return writer_blocked;}))
            throw std::runtime_error("Writer startup gate timed out");
    };
    hooks.before_write = [&](std::size_t id) {
        if (id) return;
        std::unique_lock<std::mutex> lock(mutex);
        writer_blocked = true;
        changed.notify_all();
        if (!changed.wait_for(lock,5s,[&]{return task_blocked && result_blocked;}))
            throw std::runtime_error("Queues did not both saturate");
        throw std::runtime_error("Injected cancellation with both queues full");
    };
    bool caught=false;
    try { detail::process_file_with_hooks(temp.path/"input",temp.path/"output",
            RollingXor({1,2,3}),Options{1024,2,1,12},hooks); }
    catch(const std::runtime_error& error) {
        caught=std::string(error.what())=="Injected cancellation with both queues full";
    }
    require(caught && task_blocked && result_blocked,
            "Failed to cancel a genuinely saturated pipeline");
    require_rollback(temp,temp.path/"output");
    require(read(temp.path/"input")==data,"Cancellation modified input");
}

}
int main() {
    try {
        queue_shutdown(); queue_contention(); window_cancel(); cipher_offsets();
        ordered_writer(); thread_pool_errors(); pipeline_roundtrip(); output_transaction();
        sequential_equivalence(); launch_failures(); allocation_failure();
        input_change_failures(); writer_failure(); saturated_cancellation();
        std::cout << "All 14 test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n'; return 1;
    }
}
