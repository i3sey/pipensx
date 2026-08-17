#!/usr/bin/env bash
# Leak / use-after-free audit of the UI teardown path: each screen boots in
# the sanitized golden_runner and exits through std::exit (atexit handlers +
# static destructors — the shutdown sequence main_switch.cpp runs on console
# exit). The default _exit(0) skips that sequence, so plain `make golden`
# never exercises it.
#
#   scripts/leak_check.sh                 # the representative screen set
#   LEAK_SCREENS='settings frame' scripts/leak_check.sh
#   scripts/leak_check.sh --build         # (re)build the ASan golden_runner
#
# Uses build-golden-asan/ (clang ASan+UBSan+LSan); point GOLDEN_RUNNER at a
# sanitized runner to skip the build. Known-false-positive caches from
# desktop windowing/font libraries are suppressed in scripts/lsan.supp.
set -u -o pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="${GOLDEN_RUNNER:-$ROOT/build-golden-asan/golden_runner}"
FIXTURES="$ROOT/tests/fixtures/golden"
SCREENS="${LEAK_SCREENS:-catalog downloads frame installed installed-populated updates settings storage torrent-selection bug-report}"

if [[ "${1:-}" == "--build" || ! -x "$RUNNER" ]]; then
    CC=clang CXX=clang++ cmake -S "$ROOT" -B "$ROOT/build-golden-asan" \
        -DPIPENSX_GOLDEN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie" \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -no-pie" || exit 2
    cmake --build "$ROOT/build-golden-asan" --target golden_runner || exit 2
fi

fail=0
for screen in $SCREENS; do
    out="$(mktemp /tmp/pipensx-leak-XXXX.log)"
    sandbox="$(mktemp -d /tmp/pipensx-leak-sandbox-XXXX)"
    if GOLDEN_NORMAL_EXIT=1 \
       LSAN_OPTIONS="suppressions=$ROOT/scripts/lsan.supp" \
       "$RUNNER" --fixtures "$FIXTURES" --out /tmp/pipensx-leak.png \
           --theme dark --screen "$screen" --locale en-US \
           --sandbox "$sandbox" >"$out" 2>&1; then
        echo "ok    $screen"
        rm -f "$out"
    else
        echo "FAIL  $screen: leak or sanitizer error (see $out)"
        grep -m1 -E 'ERROR: AddressSanitizer|SUMMARY: AddressSanitizer' "$out" || tail -3 "$out"
        fail=1
    fi
    rm -rf "$sandbox"
done
exit $fail
