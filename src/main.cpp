#include "filecrypt/pipeline.hpp"
#include <charconv>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
void usage(std::ostream& out) {
    out << "Usage: filecrypt <encrypt|decrypt> INPUT OUTPUT --key-file FILE\n"
        << "                 [--threads N] [--chunk-size BYTES]\n"
        << "                 [--queue-capacity N] [--max-in-flight N]\n\n"
        << "Defaults: hardware threads, 4194304-byte chunks, queues of 8, 16 in flight.\n"
        << "Output must not exist. Both modes apply the same rolling XOR transform.\n"
        << "WARNING: rolling XOR is educational, unauthenticated, and insecure.\n";
}
std::size_t positive(std::string_view value) {
    std::size_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result)
        throw std::invalid_argument("Expected a positive integer option value");
    return result;
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(std::cout); return 0; }
        if (argc < 6) { usage(std::cerr); return 2; }
        const std::string_view mode(argv[1]);
        if (mode != "encrypt" && mode != "decrypt")
            throw std::invalid_argument("Mode must be encrypt or decrypt");
        filecrypt::Options options;
        std::filesystem::path key_path;
        bool key_set = false, threads_set = false, chunk_set = false;
        bool queue_set = false, window_set = false;
        for (int i = 4; i < argc; i += 2) {
            if (i + 1 >= argc) throw std::invalid_argument("Missing option value");
            const std::string_view name(argv[i]), value(argv[i + 1]);
            auto set_number = [&](bool& seen, std::size_t& target) {
                if (seen) throw std::invalid_argument("Repeated option");
                target = positive(value);
                seen = true;
            };
            if (name == "--key-file") {
                if (key_set) throw std::invalid_argument("Repeated key-file option");
                key_set = true; key_path = argv[i + 1];
            } else if (name == "--threads") set_number(threads_set, options.workers);
            else if (name == "--chunk-size") set_number(chunk_set, options.chunk_size);
            else if (name == "--queue-capacity") set_number(queue_set, options.queue_capacity);
            else if (name == "--max-in-flight") set_number(window_set, options.max_in_flight);
            else throw std::invalid_argument("Unknown option");
        }
        if (!key_set) throw std::invalid_argument("--key-file is required");
        const filecrypt::RollingXor cipher(filecrypt::read_key(key_path));
        std::cerr << "WARNING: rolling XOR provides weak confidentiality and no authentication.\n";
        const auto start = std::chrono::steady_clock::now();
        const auto stats = filecrypt::process_file(argv[2], argv[3], cipher, options);
        const auto seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "Processed " << stats.bytes << " bytes in " << stats.chunks
                  << " chunks; workers=" << stats.workers
                  << "; peak_in_flight=" << stats.peak_in_flight
                  << "; peak_reordered=" << stats.peak_reordered
                  << "; seconds=" << std::fixed << std::setprecision(3) << seconds << '\n';
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "Argument error: " << error.what() << '\n'; return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n'; return 1;
    }
}
