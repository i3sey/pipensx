#!/usr/bin/env bash
# F4 golden-screenshot harness.
#
#   scripts/golden.sh check    - render every screen x theme, compare against
#                                tests/golden/, fail on visual regression
#   scripts/golden.sh update   - re-render and overwrite the references
#                                (single-command re-baseline)
#
# Comparison uses ImageMagick `compare -metric AE -fuzz $GOLDEN_FUZZ` with a
# pixel budget: the borealis focus-highlight pulse is wall-clock driven, so
# two identical runs already differ by ~9k px of 1280x720 (llvmpipe).
# LIBGL_ALWAYS_SOFTWARE=1 keeps local and CI rendering on the same
# rasterizer (Mesa llvmpipe); remaining AA drift is absorbed by the budget.
set -u -o pipefail

MODE="${1:-check}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE_BIN="${CMAKE_BIN:-cmake}"
RUNNER="${GOLDEN_RUNNER:-$ROOT/build-golden/golden_runner}"
FIXTURES="$ROOT/tests/fixtures/golden"
GOLDEN_DIR="$ROOT/tests/golden"
OUT_DIR="${GOLDEN_OUT:-$ROOT/build-golden/golden-out}"
FUZZ="${GOLDEN_FUZZ:-5%}"
MAX_DIFF="${GOLDEN_MAX_DIFF:-25000}"
SCREENS="${GOLDEN_SCREENS:-catalog detail frame downloads installed settings about torrent-selection}"
BEHAVIOR_SCREENS="${GOLDEN_BEHAVIOR_SCREENS:-downloads-back torrent-selection-scroll}"
THEMES="${GOLDEN_THEMES:-light dark}"

export LIBGL_ALWAYS_SOFTWARE=1

if [[ "$MODE" != "check" && "$MODE" != "update" ]]; then
    echo "usage: $0 [check|update]" >&2
    exit 2
fi
command -v compare >/dev/null || { echo "golden: ImageMagick 'compare' not found" >&2; exit 2; }

# Make the public `make golden` target work in a headless Linux shell. CI may
# already wrap this script in xvfb-run, in which case DISPLAY is set and this
# branch is skipped.
if [[ -z "${DISPLAY:-}" && "$(uname -s)" == "Linux" ]]; then
    command -v xvfb-run >/dev/null || {
        echo "golden: DISPLAY is unset and xvfb-run is not installed" >&2
        exit 2
    }
    exec xvfb-run -a "$0" "$@"
fi

echo "golden: configuring and building golden_runner"
"$CMAKE_BIN" -S "$ROOT" -B "$ROOT/build-golden" \
    -DPIPENSX_GOLDEN=ON -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 2
"$CMAKE_BIN" --build "$ROOT/build-golden" \
    --target golden_runner || exit 2

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/diff" "$GOLDEN_DIR"

fail=0
for screen in $BEHAVIOR_SCREENS; do
    name="$screen-behavior"
    if ! "$RUNNER" --fixtures "$FIXTURES" --out "$OUT_DIR/$name.png" \
                   --theme dark --screen "$screen" \
                   --sandbox "$OUT_DIR/sandbox" >"$OUT_DIR/$name.log" 2>&1; then
        echo "FAIL  $name: behavior regression (see $name.log)"
        fail=1
    else
        echo "PASS  $name"
    fi
done

for screen in $SCREENS; do
    for theme in $THEMES; do
        name="$screen-$theme"
        current="$OUT_DIR/$name.png"
        golden="$GOLDEN_DIR/$name.png"

        if ! "$RUNNER" --fixtures "$FIXTURES" --out "$current" \
                       --theme "$theme" --screen "$screen" \
                       --sandbox "$OUT_DIR/sandbox" >"$OUT_DIR/$name.log" 2>&1; then
            echo "FAIL  $name: golden_runner crashed (see $name.log)"
            fail=1
            continue
        fi

        if [[ "$MODE" == "update" ]]; then
            cp "$current" "$golden"
            echo "BASE  $name"
            continue
        fi

        if [[ ! -f "$golden" ]]; then
            echo "FAIL  $name: no reference tests/golden/$name.png (run scripts/golden.sh update)"
            fail=1
            continue
        fi

        ae="$(compare -metric AE -fuzz "$FUZZ" "$golden" "$current" \
                      "$OUT_DIR/diff/$name.png" 2>&1 | awk '{print $1}')"
        if ! [[ "$ae" =~ ^[0-9]+([.][0-9]+([eE][+-][0-9]+)?)?$ ]]; then
            echo "FAIL  $name: compare error: $ae"
            fail=1
        elif [[ "${ae%%.*}" -gt "$MAX_DIFF" ]]; then
            echo "FAIL  $name: $ae px differ (budget $MAX_DIFF, fuzz $FUZZ)"
            fail=1
        else
            echo "ok    $name: $ae px within budget $MAX_DIFF"
            rm -f "$OUT_DIR/diff/$name.png"
        fi
    done
done

if [[ "$MODE" == "check" && "$fail" -ne 0 ]]; then
    echo "golden: visual regression detected; diffs in $OUT_DIR/diff," \
         "re-baseline with scripts/golden.sh update" >&2
fi
exit "$fail"
