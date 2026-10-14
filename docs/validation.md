# Validation record

Executed on 15 October 2026 using horker.

## Environments

- Windows: MinGW-w64 GCC 15.2.0, CMake, Ninja, NTFS staging/publication.
- Linux: Ubuntu 24.04 under WSL2, GCC 13.3.0, CMake, Ninja.
- Language: C++17. No external core libraries or test framework.
- Release compilation completed with the configured warning flags and no compiler warnings.

## Results

| Check | Result |
| --- | --- |
| Windows Release build and CTest | Passed, all eight test groups |
| Linux Release build and CTest | Passed, all eight test groups |
| ASan + UBSan + leak detection CTest | Passed, all eight test groups; no diagnostics |
| Release 50 MiB random file × 100 round trips | Passed; SHA-256 and exact bytes matched every iteration |
| ASan/UBSan 50 MiB random file × 10 round trips | Passed; no sanitizer diagnostics |
| CLI invalid options, binary keys, spaces, existing/aliased/dangling destinations | Passed |
| Mid-pipeline write failure with active reader/workers | Passed; exit 1, no published output, no leftover staging directory |
| 256 MiB file under 128 MiB virtual-address limit | Passed encrypt/decrypt and byte comparison |
| TSan unit tests with per-process ASLR workaround | Passed, all eight groups; no race diagnostics |
| TSan 50 MiB random file × 100 round trips | Passed with the per-process workaround; hashes/bytes matched, no race diagnostics |

Release fixture SHA-256:

```text
8677160ffbc25cb0132bc8098331910552db1428c16786ad1643b0f26d6877af
```

ASan/UBSan stress fixture SHA-256:

```text
040520a31842d567311a25cd1fc55422cde5458e5d58b975a870aaec015d3a44
```

TSan stress fixture SHA-256:

```text
37ac3a02bbd51ad25d61f46b92f86be52b596f7cc3ac76f82ef58fa70f5df502
```

Bounded-memory fixture/recovered SHA-256:

```text
a6d72ac7690f53be6ae46ba88506bd97302a093f7108472bd9efc3cefda06484
```

The bounded-memory run used two workers, 1 MiB chunks, queue capacity 1,
and a limit of four chunks in flight. Both directions reported peak_in_flight=4
and peak_reordered=2. The input exceeded the process address-space limit,
demonstrating that whole-file buffering was not required.

## TSan host limitation and workaround

The ordinary TSan invocation built successfully but stopped before main:

```text
FATAL: ThreadSanitizer: unexpected memory mapping
```

The same executable ran successfully using per-process address randomization
control, without changing a global system setting:

```bash
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 ./build-tsan/filecrypt_tests
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=1 bash test.sh ./build-tsan/filecrypt
```

## Coverage and practical limits

The eight C++ test groups cover queue backpressure/drain/cancel and move-only
ownership, concurrent queue traffic, credit-window cancellation, a known cipher
reference across multiple keys/chunk partitions/reverse processing order, forced
writer reordering and write errors, worker exception propagation, file round trips,
and output rollback/publication races.

Test hashes demonstrate integrity of these workloads. Clean sanitizers support
the exercised paths and schedules; they cannot prove every possible execution.
The rolling XOR cipher remains cryptographically insecure even when every
integrity test passes.

The large-file check used 256 MiB rather than writing a 10+ GiB fixture.
64-bit offsets and the in-flight payload invariant support larger files subject
to disk capacity and the documented filesystem contract. No 10 GiB throughput
benchmark or power-loss durability test is claimed.

The GitHub Actions workflow is included for future runs. Local test results
above are independent of the eventual hosted workflow status.
