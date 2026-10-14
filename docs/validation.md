# Validation record

## Systems improvements validated on 15 October 2026

The implementation at the improvement commits adds a true sequential engine,
bounded buffer recycling, metrics, and deterministic fault-boundary tests.

| Check | Result |
| --- | --- |
| Windows MinGW GCC 15.2.0 Release build/CTest | Passed, all 14 groups |
| Linux GCC 13.3.0 Release build/CTest | Passed, all 14 groups |
| ASan + UBSan + leak detection CTest | Passed, all 14 groups, no diagnostics |
| TSan CTest using per-process ASLR workaround | Passed, all 14 groups, no diagnostics |
| Updated Release stress: 50 MiB × 100 round trips | Passed hashes and exact bytes |
| Updated ASan/UBSan stress: 50 MiB × 10 round trips | Passed, no diagnostics |
| Updated TSan stress: 50 MiB × 10 round trips | Passed, no race diagnostics |
| Sequential/pipeline CLI equivalence and invalid engine arguments | Passed |
| Actual file-size-limit write failure and staging cleanup | Passed |
| 256 MiB round trip under 128 MiB address-space limit | Passed, four payload allocations per direction |
| Randomized before/pooled benchmark matrix | 150 measured outputs verified; five samples/configuration |
| 11 GiB + 123 byte round trip under 256 MiB address-space limit | Passed SHA-256 and exact byte comparison |

New fault tests cover partial worker creation, producer-thread creation,
payload allocation failure with an early worker held, real source shrink/growth,
writer failure after successful writes, and writer failure while BOTH queues
have actually blocked producers. They assert original-error propagation,
thread termination, source preservation where the test does not deliberately
mutate it, absence of published output, and removal of staging directories.

Allocation and launch failures are injected immediately before the corresponding
native operations. They exercise the real cleanup paths; this is not a claim of
physically exhausting the OS thread allocator or all system memory.
Queue saturation is coordinated by condition variables and observed wait events,
rather than relying on a scheduling delay.

## Large-file evidence

- Logical input: **11811160187 bytes (11 GiB + 123)**.
- WSL host RAM: 7924772864 bytes; the input was larger than total host RAM.
- Child-process virtual-address limit: 268435456 bytes (256 MiB).
- Peak resident memory: **20348 KiB (19.87 MiB)** in each direction.
- Two crypto workers, 4 MiB chunks, window 4, queue capacity 1.
- Each direction processed 2817 chunks and allocated four payload buffers.
- Linux ext4 fixture: zero-filled sparse file with three random 64 KiB markers;
  204800 input bytes were physically allocated.
- Every logical byte was read/transformed/written by the pipeline. The outputs
  were ordinary full sequential writes; this is not a file-copy shortcut.
- SHA-256 and a separate exact cmp check both succeeded.
- Temporary input/output/key/staging fixtures were removed.

Original and recovered SHA-256:

```text
c78949c75208459bad1a8e3f011bfde733930f54505e60fa75c701d5b92f7711
```

[Raw large-file record](benchmarks/2026-10-15/large-file.json).
The earlier Windows-mounted large trial was deliberately interrupted because
of its I/O overhead and cleaned up; it is not counted as a successful validation.
The successful large-file run used native Linux storage. Windows validation
covers the implementation and failure tests, not an 11 GiB Windows workload.

## Performance evidence and limits

[Performance report](performance.md) retains the full before/after table,
[raw samples](benchmarks/2026-10-15/raw.csv), min/max ranges, cache policy,
binary hashes, and environment. Buffer reuse reduces allocation counts
predictably; measured throughput has improvements AND regressions. No universal
speedup or physical disk bandwidth claim is made. Large-run timings are a
single integrity/memory test, not a dedicated throughput benchmark.

The hosted workflow now includes an MSVC Windows job and a benchmark smoke test.
Local Windows results above used MinGW; hosted job completion is not inferred.

Updated stress fixture SHA-256 values:

```text
Release: 39f103f2d61e8876456c81aa603384d794b52bf155129703a551a500a2ee93f2
ASan:    b80d57646448e9ea4d8430da5b8c057bb3790743ed64c758abaa9cdf58857a46
TSan:    6d20d7af4d84a302447a0764172fe9c750629180c9ce8db66bde4e1e8c7cfb06
```

Rolling XOR remains insecure and unauthenticated. These improvements target
systems engineering and do not introduce a secure encryption backend.

---

## Initial release record (commit 1b20e6e)

The following results describe the initial implementation before the improvements.

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
