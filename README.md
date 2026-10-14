# filecrypt

A dependency-free C++17 command-line file transformer with a bounded
producer/worker/writer pipeline. Built with RAII, blocking condition variables,
independent chunk processing, and strictly ordered output.

**The requested rolling-key XOR is an educational baseline, not secure encryption.**
It has a predictable repeating keystream, no nonce, no authentication, and no
wrong-key detection. Do not use it to protect sensitive data. The resource and
concurrency architecture is suitable for a real authenticated cipher adapter,
but that replacement requires cryptographic design and a vetted implementation.

## Source tree

```text
filecrypt/
├── CMakeLists.txt
├── include/filecrypt/
│   ├── chunk.hpp                 # Chunk ownership and EOF metadata
│   ├── thread_safe_queue.hpp     # Bounded blocking queue with drain/cancel
│   ├── in_flight_window.hpp      # Whole-pipeline memory budget
│   ├── buffer_pool.hpp           # Bounded reusable payload storage
│   ├── detail/runtime_hooks.hpp  # Deterministic test boundaries
│   ├── thread_pool.hpp           # RAII thread ownership
│   ├── crypto.hpp                # Immutable cipher interface and key loading
│   ├── file_io.hpp               # Reader, ordered writer, output transaction
│   └── pipeline.hpp              # Configuration and orchestration
├── src/
│   ├── main.cpp                  # Argument parsing and CLI
│   ├── thread_pool.cpp
│   ├── crypto.cpp
│   ├── file_io.cpp
│   └── pipeline.cpp
├── tests/
│   ├── tests.cpp                 # Fourteen unit/integration test groups
│   ├── cli.sh                    # CLI errors and real write-failure cancellation
│   ├── large_file.py             # 11 GiB test with RSS/address-space limits
│   └── memory_bound.sh           # Input larger than a process memory limit
├── test.sh                       # 50 MiB × 100 hash/byte-verified round trips
├── benchmarks/run.py            # Repeated sequential/pipeline comparison
├── docs/performance.md
├── docs/architecture.md
├── docs/validation.md
└── .github/workflows/ci.yml       # Release, ASan/UBSan, and TSan jobs
```

Every header/source is complete; there are no external core libraries or
generated dependencies. Bash tests need ordinary Unix command-line tools.

## Build

Requires CMake 3.16+, a C++17 compiler, and the platform thread runtime.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
bash tests/cli.sh ./build/filecrypt
bash test.sh ./build/filecrypt
```

Release uses `-O3` on GCC/Clang and `/O2` on MSVC. C++20 can also compile
the source; the project targets C++17. Windows supports the core and CTest
with MSVC or MinGW; run bash/sanitizer tests on Linux or WSL.

## Use

Store the key as a binary file, keeping it out of source control:

```bash
umask 077
head -c 32 /dev/urandom > secret.key
./build/filecrypt encrypt input.bin encrypted.bin --key-file secret.key
./build/filecrypt decrypt encrypted.bin recovered.bin --key-file secret.key
sha256sum input.bin recovered.bin
```

Both modes apply the same reversible transform. Key bytes are read exactly,
including zeros and newlines; no trimming or password derivation occurs.
Keys must be nonempty regular files of at most 1 MiB.

| Option | Default | Meaning |
| --- | --- | --- |
| `--engine pipeline\|sequential` | pipeline | Worker pipeline or truly single-threaded execution |
| `--threads N` | hardware concurrency, or 1 | Number of crypto workers |
| `--chunk-size BYTES` | 4194304 (4 MiB) | Maximum bytes in each chunk |
| `--queue-capacity N` | 8 | Capacity of each of the two queues |
| `--max-in-flight N` | 16 | Maximum total admitted chunks across all stages |

All explicit numeric values must be positive. For example:

```bash
./build/filecrypt encrypt input.bin output.bin --key-file secret.key \
  --threads 4 --chunk-size 1048576 --queue-capacity 2 --max-in-flight 8
```

Cipher output is identical across thread counts, completion order, and chunk
sizes. Decryption may use a different chunk size. Output contains only transformed
bytes; it has no header, key identifier, checksum, or authentication tag.

Exit status: `0` success; `1` processing/I/O/resource failure; `2` invalid arguments.
Summary output includes bytes, chunks, worker count, elapsed time, observed
peak in-flight/pending chunks, payload buffer allocations, and queue push waits. Existing outputs are always refused, including
symlinks and aliases of the input.

## Memory and throughput

At most `max_in_flight × chunk_size` payload bytes are owned by the whole
pipeline: **64 MiB with defaults**, independent of file size and worker count.
Allow additional memory for the key, stream buffers, allocator/map/queue metadata,
the executable, and worker thread stacks. This is a payload bound, not a promise
of a 64 MiB process RSS. Lower worker count when thread-stack overhead matters.

Chunks move between containers; their payloads are transformed in place.
The writer returns buffers to a bounded pool before returning admission credits.
The pool keeps at most the in-flight limit of payload buffers, including idle
buffers; it avoids repeated allocation and initialization during full reads.
The sequential reader and writer each own their stream; workers only touch their
own chunks and a shared immutable key. No full-file buffering or busy polling
occurs. Tune chunk size/window against the storage device and memory budget.
Hardware concurrency is the requested default; spawning many workers does not
guarantee better throughput when storage or memory bandwidth is the bottleneck.

## Sequential baseline and performance

`--engine sequential` uses one reusable buffer and starts no worker, producer,
or writer threads. A pipeline configured with `--threads 1` still has separate
producer and consumer stages, so it is a different measurement.

Both engines use identical chunk offset calculations, read checks, flush/close,
staging permissions, and atomic publication. The sequential baseline ignores
worker/queue/window tuning after validating their values.

Linux benchmark and optional large-file validation (Python 3 and GNU time):

```bash
python3 benchmarks/run.py --binary current=build/filecrypt \
  --output test-artifacts/benchmark --size-mib 512 --repeats 5
python3 tests/large_file.py ./build/filecrypt --size-gib 11 \
  --address-limit-mib 256 --temp-dir /tmp \
  --output test-artifacts/large-file.json
```

The large test reserves space for full outputs, uses zero-filled input with random
markers (sparse where supported), verifies hashes and exact bytes, and automatically removes fixtures.
Use a native Linux filesystem for fixtures; Windows-mounted paths in WSL can
add substantial I/O overhead. The temporary parent directory must exist. It is optional
because it writes more than 22 GiB. Use a release build; sanitizer runtimes
require large virtual address spaces.

[Measured results and methodology](docs/performance.md) include raw repeated
measurements, memory use, before/after buffer-reuse comparisons, and limitations.

## Sanitizers and stress tests

ASan and TSan are mutually exclusive and require separate build directories.
Sanitizers use `-O1 -g -fno-omit-frame-pointer` for diagnostics.

```bash
cmake -S . -B build-asan -DFILECRYPT_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  bash test.sh ./build-asan/filecrypt

cmake -S . -B build-tsan -DFILECRYPT_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan --parallel
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure
TSAN_OPTIONS=halt_on_error=1 bash test.sh ./build-tsan/filecrypt
```

`test.sh` generates a 50 MiB random fixture and runs **100 independent encrypt/
decrypt cycles**, checking SHA-256 (MD5 fallback) and exact byte equality on
every iteration. It varies queue/window/thread settings and chunk partitions,
including queue capacity 1 and more workers than available credits.
Each transformation has a deadline to surface hangs.
Override `ITERATIONS`, `SIZE_MB`, or `TIMEOUT_SECONDS` for other workloads.

`bash tests/memory_bound.sh ./build/filecrypt` additionally processes a 256 MiB
file under a 128 MiB address-space limit. Use a release build on Linux; sanitizer
runtimes need large virtual address spaces and cannot use that limit.

On a host where TSan exits before main with "unexpected memory mapping", a
per-process diagnostic workaround (where supported) is:

```bash
setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-tsan --output-on-failure
setarch "$(uname -m)" -R env TSAN_OPTIONS=halt_on_error=1 \
  bash test.sh ./build-tsan/filecrypt
```

This changes address randomization for these test processes only. It may be
unavailable in restricted containers; use a supported sanitizer host instead.
The CI TSan job uses this workaround.

Checksums establish round-trip integrity, not the absence of races. TSan detects
races exercised by a test; neither a clean run nor the stress test proves that all
possible schedules are correct. See [validation](docs/validation.md) for actual
results and environment limitations.

## Filesystem and operational contract

The input must be a stable regular file while processing. Size changes are
detected at read boundaries; concurrent same-size modifications cannot be
reliably detected with standard streams. Obtain a snapshot or external lock if
other processes may write the source.

Output is written to a private sibling staging directory. After every participant
has joined and flush/close have succeeded, a hard link publishes the completed
file without replacing a raced destination. The filesystem must support hard
links; NTFS and ordinary Linux filesystems do. Unsupported filesystems fail
safely. Staging uses file permissions supported by the platform; Windows access
control remains governed by inherited ACLs.

Ordinary C++ exception/I/O failures remove staging data through RAII and preserve
the source and any existing destination. Forced termination, machine failure,
or process-killing signals can leave a `.filecrypt-*` directory for manual cleanup.
Standard C++ has no portable durable sync primitive; flush/close and publication
do not promise power-loss durability. Keep sufficient disk space for the full
output. No secure key erasure or memory locking is claimed.

See [architecture](docs/architecture.md) for the ordering, memory, and shutdown
invariants.
