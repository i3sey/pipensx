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
SCREENS="${GOLDEN_SCREENS:-catalog detail frame downloads installed settings about torrent-selection bug-report bug-report-detail}"
# Behaviour checks: these assert and exit non-zero instead of writing a
# baseline, so they are never compared against tests/golden/. Entries are
# <screen> or <screen>:<locale>; hints-budget runs in both because Russian hint
# labels are ~20% wider than English and are what actually overruns the bar.
BEHAVIOR_SCREENS="${GOLDEN_BEHAVIOR_SCREENS:-downloads-back torrent-selection-scroll hints-budget hints-budget:ru bug-report-focus}"
THEMES="${GOLDEN_THEMES:-light dark}"
# frame is in the list because it is the only screen that renders the nav
# sidebar, whose 248px width (theme.hpp installSidebarStyle) is the
# tightest label constraint in the app.
RU_SCREENS="${GOLDEN_RU_SCREENS:-frame catalog detail settings torrent-selection downloads}"
RU_THEME="${GOLDEN_RU_THEME:-dark}"

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
for entry in $BEHAVIOR_SCREENS; do
    screen="${entry%%:*}"
    locale="en-US"
    [[ "$entry" == *:* ]] && locale="${entry##*:}"
    name="${entry//:/-}-behavior"
    if ! "$RUNNER" --fixtures "$FIXTURES" --out "$OUT_DIR/$name.png" \
                   --theme dark --screen "$screen" --locale "$locale" \
                   --sandbox "$OUT_DIR/sandbox" >"$OUT_DIR/$name.log" 2>&1; then
        echo "FAIL  $name: behavior regression (see $name.log)"
        fail=1
    else
        echo "PASS  $name"
    fi
done

# render_and_compare <name> <screen> <theme> <locale>
render_and_compare() {
    local name="$1" screen="$2" theme="$3" locale="$4"
    local current="$OUT_DIR/$name.png"
    local golden="$GOLDEN_DIR/$name.png"

    if ! "$RUNNER" --fixtures "$FIXTURES" --out "$current" \
                   --theme "$theme" --screen "$screen" --locale "$locale" \
                   --sandbox "$OUT_DIR/sandbox" >"$OUT_DIR/$name.log" 2>&1; then
        echo "FAIL  $name: golden_runner crashed (see $name.log)"
        fail=1
        return
    fi

    if [[ "$MODE" == "update" ]]; then
        cp "$current" "$golden"
        echo "BASE  $name"
        return
    fi

    if [[ ! -f "$golden" ]]; then
        echo "FAIL  $name: no reference tests/golden/$name.png (run scripts/golden.sh update)"
        fail=1
        return
    fi

    local ae
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
}

for screen in $SCREENS; do
    for theme in $THEMES; do
        render_and_compare "$screen-$theme" "$screen" "$theme" en-US
    done
done

# Russian pass. Guards against clipped labels only — Russian strings run
# 15-30% longer than English and much of the UI is setSingleLine(true) or a
# fixed width. Text-dense screens only, and one theme, because clipping is
# theme-independent: a full mirror would double the re-baseline cost of every
# future UI change for no extra signal.
for screen in $RU_SCREENS; do
    render_and_compare "ru-$screen-$RU_THEME" "$screen" "$RU_THEME" ru
done

if [[ "$MODE" == "check" && "$fail" -ne 0 ]]; then
    echo "golden: visual regression detected; diffs in $OUT_DIR/diff," \
         "re-baseline with scripts/golden.sh update" >&2
fi
exit "$fail"
