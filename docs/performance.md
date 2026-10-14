# Performance and resource measurements

Measured on 15 October 2026 through horker. This report includes improvements
and regressions; it does not claim that buffer pooling always increases speed.

## Reproducible method

- AMD Ryzen 7 7735HS, 8 cores/16 logical CPUs; Ubuntu 24.04 under WSL2.
- GCC 13.3.0, C++17, Release -O3; same rolling XOR algorithm and 257-byte key.
- 512 MiB random fixture shared between the two executable variants.
- 64 KiB, 1 MiB, and 4 MiB chunks; 1, 2, 4, and 8 pipeline crypto workers.
- True sequential baseline: all stages run on one thread with one reusable buffer.
- Queue capacity 4 and in-flight limit 8. One untimed warm-up per configuration.
- Five measured runs per variant/configuration, randomized and interleaved:
  **150 measured transformations**, all verified against the sequential ciphertext.
- Process launch through flush/close and atomic publication is timed.
  Fixture generation, hashing, and verification are outside the measured interval.
- GNU time measures child peak RSS, user CPU time, and system CPU time.
- Warm input cache, ext4 inside WSL, buffered writes, no fsync/cache dropping.

The source before pooling is commit
[0fb1cde](https://github.com/DenishShiroya22/filecrypt/commit/0fb1cde650dff0f0c5047c19533ac7eef97b0029).
The pooled variant adds BufferPool and returns storage before returning credits.
[Executable hashes/environment](benchmarks/2026-10-15/environment.json),
[raw measurements](benchmarks/2026-10-15/raw.csv), and
[summary with min/max ranges](benchmarks/2026-10-15/summary.csv) are retained.

## Results

Numbers below are medians of five samples; RSS is the maximum among those samples.

| Chunk | Workers | Before MiB/s | Pooled MiB/s | Median change | Before/pooled max RSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 KiB | 1 | 345.3 | 340.3 | -1.5% | 4.8 / 4.8 |
| 1024 KiB | 1 | 492.3 | 534.4 | +8.6% | 13.3 / 12.0 |
| 4096 KiB | 1 | 518.8 | 551.8 | +6.4% | 40.2 / 36.0 |
| 64 KiB | 2 | 489.6 | 554.5 | +13.2% | 4.7 / 4.8 |
| 1024 KiB | 2 | 844.6 | 931.4 | +10.3% | 13.3 / 12.0 |
| 4096 KiB | 2 | 873.1 | 931.8 | +6.7% | 40.3 / 36.0 |
| 64 KiB | 4 | 426.3 | 955.1 | +124.0% | 4.7 / 4.7 |
| 1024 KiB | 4 | 1330.6 | 915.8 | -31.2% | 13.2 / 12.0 |
| 4096 KiB | 4 | 1123.4 | 1059.2 | -5.7% | 40.3 / 36.0 |
| 64 KiB | 8 | 811.6 | 943.7 | +16.3% | 4.8 / 4.9 |
| 1024 KiB | 8 | 1440.3 | 1107.5 | -23.1% | 13.4 / 12.1 |
| 4096 KiB | 8 | 1238.1 | 914.6 | -26.1% | 40.3 / 36.1 |

The pooled sequential baseline medians were **408.0, 294.5, and 322.5 MiB/s**
for 64 KiB, 1 MiB, and 4 MiB chunks respectively. The 8-worker, 1 MiB pooled
pipeline median was **1107.5 MiB/s**, about **3.76 times** the matching sequential
baseline on this workload. This is a buffered warm-cache application measurement,
not physical disk bandwidth or a prediction for authenticated encryption.

## Allocation and memory tradeoff

For 512 MiB input and an eight-chunk window:

| Chunk size | Before payload allocations | Pooled payload allocations |
| --- | ---: | ---: |
| 64 KiB | 8192 | 8 |
| 1 MiB | 512 | 8 |
| 4 MiB | 128 | 8 |

Pooling retains reusable storage, including idle vectors, within the same
window-sized payload bound. It reduces vector growth events and redundant
initialization; at 4 MiB, observed peak RSS fell from about 40.3 MiB to 36.1 MiB
in the 8-worker configuration. The key, metadata, runtime, and thread stacks
remain outside the payload budget.

This is retained as an allocation/resource optimization. It is **not marketed
as a universal throughput improvement**: several medians improved, several
regressed, and sample ranges were wide and often overlapped. Even the unchanged
sequential path varied between the executable variants. A shared laptop/WSL
environment cannot isolate host activity, thermal/frequency changes, or cache
effects. A dedicated host and longer repeated trials are needed before changing
defaults or drawing causal conclusions about small timing differences.

## Reproduce

Build the current source and run:

```bash
python3 benchmarks/run.py --binary current=build/filecrypt \
  --output test-artifacts/benchmark --size-mib 512 --repeats 5
```

For a comparison, build the before commit in a separate checkout, then supply
both binaries to the harness:

```bash
python3 benchmarks/run.py --binary before=/path/to/before/filecrypt \
  --binary pooled=/path/to/current/filecrypt \
  --output test-artifacts/comparison --size-mib 512 --repeats 5
```

Every report directory must be new. The harness uses a private temporary
directory, checks every output hash, and deletes fixtures after completion.

The separate large-file memory/integrity evidence is recorded in
[validation](validation.md). The rolling XOR security limitation remains
unchanged.
