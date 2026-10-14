#!/usr/bin/env bash
# Defaults: exactly 100 independent round trips of a 50 MiB random fixture.
# Usage: ./test.sh [path/to/filecrypt]
set -euo pipefail
export LC_ALL=C
binary="${1:-./build/filecrypt}"
iterations="${ITERATIONS:-100}"
size_mb="${SIZE_MB:-50}"
timeout_seconds="${TIMEOUT_SECONDS:-120}"
for value in "$iterations" "$size_mb" "$timeout_seconds"; do
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "Expected positive integer settings" >&2; exit 2; }
done
[[ -x "$binary" ]] || { echo "Build filecrypt first: $binary" >&2; exit 2; }
if command -v sha256sum >/dev/null 2>&1; then hash_tool=sha256sum
elif command -v md5sum >/dev/null 2>&1; then hash_tool=md5sum
else echo "sha256sum or md5sum is required" >&2; exit 2
fi
command -v timeout >/dev/null 2>&1 || { echo "GNU timeout is required" >&2; exit 2; }
umask 077
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
dd if=/dev/urandom of="$work/input.bin" bs=1M count="$size_mb" status=none
dd if=/dev/urandom of="$work/key.bin" bs=257 count=1 status=none
hash() { "$hash_tool" "$1" | cut -d ' ' -f 1; }
expected="$(hash "$work/input.bin")"
run() {
    if ! timeout --kill-after=5s "${timeout_seconds}s" "$binary" "$@" >"$work/run.log" 2>&1; then
        cat "$work/run.log" >&2
        echo "Transformation failed or exceeded deadline" >&2
        exit 1
    fi
}
for ((iteration=1; iteration<=iterations; ++iteration)); do
    # Exercise capacity 1, a worker count larger than the window, exact/partial
    # final chunks, and independent encryption/decryption partitions.
    case $((iteration % 4)) in
        0) enc_opts=(); dec_opts=() ;;
        1) enc_opts=(--threads 8 --queue-capacity 1 --max-in-flight 3 --chunk-size 65537)
           dec_opts=(--threads 2 --queue-capacity 1 --max-in-flight 1 --chunk-size 1048576) ;;
        2) enc_opts=(--threads 1 --queue-capacity 2 --max-in-flight 2 --chunk-size 4194304)
           dec_opts=(--threads 4 --queue-capacity 2 --max-in-flight 5 --chunk-size 4194303) ;;
        3) enc_opts=(--threads 4 --queue-capacity 1 --max-in-flight 8 --chunk-size 1048576)
           dec_opts=(--threads 8 --queue-capacity 1 --max-in-flight 3 --chunk-size 65537) ;;
    esac
    run encrypt "$work/input.bin" "$work/encrypted.bin" --key-file "$work/key.bin" "${enc_opts[@]}"
    run decrypt "$work/encrypted.bin" "$work/decrypted.bin" --key-file "$work/key.bin" "${dec_opts[@]}"
    actual="$(hash "$work/decrypted.bin")"
    [[ "$actual" == "$expected" ]] || { echo "Hash mismatch on iteration $iteration" >&2; exit 1; }
    cmp -s "$work/input.bin" "$work/decrypted.bin" || { echo "Byte mismatch" >&2; exit 1; }
    rm -f -- "$work/encrypted.bin" "$work/decrypted.bin"
    if (( iteration % 10 == 0 || iteration == iterations )); then
        echo "PASS $iteration/$iterations"
    fi
done
echo "PASS: $iterations round trips of $size_mb MiB; $hash_tool=$expected"
# Hashes demonstrate integrity; run a TSan build to dynamically detect races.
