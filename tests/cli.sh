#!/usr/bin/env bash
set -euo pipefail
binary="${1:-./build/filecrypt}"
[[ -x "$binary" ]] || { echo "Missing executable: $binary" >&2; exit 2; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
printf 'binary key\000\377' > "$work/key.bin"
printf 'fixture with spaces' > "$work/input with spaces.bin"
expect_exit() {
    local expected="$1"; shift
    local actual=0
    timeout --kill-after=3s 20s "$binary" "$@" > "$work/log" 2>&1 || actual=$?
    if [[ "$actual" != "$expected" ]]; then
        cat "$work/log" >&2
        echo "Expected exit $expected, got $actual" >&2; exit 1
    fi
}
expect_exit 0 --help
expect_exit 2
expect_exit 2 invalid "$work/input with spaces.bin" "$work/out" --key-file "$work/key.bin"
expect_exit 2 encrypt "$work/input with spaces.bin" "$work/out" --key-file "$work/key.bin" --threads 0
expect_exit 2 encrypt "$work/input with spaces.bin" "$work/out" --key-file "$work/key.bin" --threads 2 --threads 3
expect_exit 2 encrypt "$work/input with spaces.bin" "$work/out" --key-file "$work/key.bin" --chunk-size abc
expect_exit 2 encrypt "$work/input with spaces.bin" "$work/out" --key-file "$work/key.bin" --threads
expect_exit 2 encrypt "$work/input with spaces.bin" "$work/out" --key-file "$work/key.bin" --unknown 2
expect_exit 2 encrypt "$work/missing" "$work/out" --key-file "$work/key.bin"
[[ ! -e "$work/out" ]]
expect_exit 1 encrypt "$work/input with spaces.bin" "$work/input with spaces.bin" --key-file "$work/key.bin"
ln "$work/input with spaces.bin" "$work/alias"
expect_exit 1 encrypt "$work/input with spaces.bin" "$work/alias" --key-file "$work/key.bin"
ln -s "$work/nonexistent" "$work/dangling"
expect_exit 1 encrypt "$work/input with spaces.bin" "$work/dangling" --key-file "$work/key.bin"
expect_exit 0 encrypt "$work/input with spaces.bin" "$work/out" --key-file "$work/key.bin" --threads 2
expect_exit 0 decrypt "$work/out" "$work/recovered" --key-file "$work/key.bin" --chunk-size 7 --threads 4
cmp "$work/input with spaces.bin" "$work/recovered"
# Real output failure with active producer/workers. SIGXFSZ is ignored only in
# this subprocess so writes report an error instead of killing the process.
dd if=/dev/zero of="$work/large.bin" bs=1M count=8 status=none
actual=0
(
    trap '' XFSZ
    ulimit -f 1024
    timeout --kill-after=3s 20s "$binary" encrypt "$work/large.bin" "$work/failed-output" \
        --key-file "$work/key.bin" --threads 8 --chunk-size 65536 \
        --queue-capacity 1 --max-in-flight 3
) > "$work/failure.log" 2>&1 || actual=$?
if [[ "$actual" != 1 ]]; then cat "$work/failure.log" >&2; echo "Write failure did not exit cleanly: $actual" >&2; exit 1; fi
[[ ! -e "$work/failed-output" ]]
if find "$work" -maxdepth 1 -name '.filecrypt-*' | read -r leftover; then
    echo "Write failure leaked staging directory: $leftover" >&2; exit 1
fi
echo "PASS: CLI validation, aliases, binary keys, and write-failure cancellation"
