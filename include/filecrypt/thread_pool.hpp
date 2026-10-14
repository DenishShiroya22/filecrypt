#pragma once
#include <cstddef>
#include <exception>
#include <functional>
#include <thread>
#include <vector>

namespace filecrypt {
// Owns and joins all threads, including when launching a later thread fails.
// The error callback MUST be nonthrowing and wake blocked work before joining.
class ThreadPool {
public:
    using Task = std::function<void()>;
    using OnError = std::function<void(std::exception_ptr)>;
    ThreadPool(std::size_t count, Task task, OnError on_error);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    void join();
private:
    std::vector<std::thread> threads_;
};
} // namespace filecrypt
