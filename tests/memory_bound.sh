#!/usr/bin/env bash
# Release build only: sanitizer shadow memory conflicts with an address limit.
set -euo pipefail
binary="${1:-./build/filecrypt}"
[[ -x "$binary" ]] || { echo "Missing executable" >&2; exit 2; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
umask 077
# Sparse input still forces the pipeline to read/write every byte.
truncate -s 256M "$work/input.bin"
head -c 32 /dev/urandom > "$work/key.bin"
(
    ulimit -v 131072
    timeout --kill-after=3s 120s "$binary" encrypt "$work/input.bin" "$work/encrypted" \
        --key-file "$work/key.bin" --threads 2 --chunk-size 1048576 --max-in-flight 4 --queue-capacity 1
    timeout --kill-after=3s 120s "$binary" decrypt "$work/encrypted" "$work/decrypted" \
        --key-file "$work/key.bin" --threads 2 --chunk-size 1048576 --max-in-flight 4 --queue-capacity 1
)
cmp "$work/input.bin" "$work/decrypted"
sha256sum "$work/input.bin" "$work/decrypted"
echo "PASS: 256 MiB round trip under a 128 MiB process address-space limit"
