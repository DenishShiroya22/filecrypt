#include "filecrypt/thread_pool.hpp"
#include <stdexcept>
#include <utility>

namespace filecrypt {
ThreadPool::ThreadPool(std::size_t count, Task task, OnError on_error) {
    if (!count) throw std::invalid_argument("Thread count must be positive");
    threads_.reserve(count);
    try {
        for (std::size_t i = 0; i < count; ++i) {
            threads_.emplace_back([task, on_error] {
                try { task(); }
                catch (...) { on_error(std::current_exception()); }
            });
        }
    } catch (...) {
        on_error(std::current_exception());
        join();
        throw;
    }
}
ThreadPool::~ThreadPool() { join(); }
void ThreadPool::join() {
    for (auto& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
}
} // namespace filecrypt
