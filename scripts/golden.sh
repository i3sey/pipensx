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
SCREENS="${GOLDEN_SCREENS:-catalog detail frame downloads download-files deploy-preview installed installed-populated updates update-chooser settings settings-debrid storage network-health help first-run debrid-link about torrent-selection bug-report bug-report-detail screenshot-viewer screenshot-viewer-preview screenshot-viewer-missing}"
# Behaviour checks: these assert and exit non-zero instead of writing a
# baseline, so they are never compared against tests/golden/. Entries are
# <screen> or <screen>:<locale>; hints-budget runs in both because Russian hint
# labels are ~20% wider than English and are what actually overruns the bar.
BEHAVIOR_SCREENS="${GOLDEN_BEHAVIOR_SCREENS:-downloads-back downloads-removing torrent-selection-scroll hints-budget hints-budget:ru bug-report-focus first-run-focus first-run-focus:ru sidebar-touch detail-rail-nav catalog-header-clearance update-chooser-toggle first-run-disclaimer installed-bundles}"
THEMES="${GOLDEN_THEMES:-light dark}"
# frame is in the list because it is the only screen that renders the nav
# sidebar, whose 280px width (main_frame.hpp installSidebarStyle) is the
# tightest label constraint in the app.
RU_SCREENS="${GOLDEN_RU_SCREENS:-frame catalog detail settings settings-debrid help first-run debrid-link torrent-selection downloads download-files deploy-preview updates update-chooser}"
RU_THEME="${GOLDEN_RU_THEME:-dark}"

export LIBGL_ALWAYS_SOFTWARE=1

if [[ "$MODE" != "check" && "$MODE" != "update" ]]; then
    echo "usage: $0 [check|update]" >&2
    exit 2
fi
command -v compare >/dev/null || { echo "golden: ImageMagick 'compare' not found" >&2; exit 2; }

# Always render on a private X server, not the developer's session. Each of the
# ~40 renders opens a real 1280x720 SDL window: on a desktop that flashes
# windows, steals keyboard focus mid-run, and a stray keypress reaches the
# runner and perturbs the screenshot. Also makes `make golden` work in a
# headless shell, which is how CI runs it. GOLDEN_HEADLESS=0 opts out to watch
# the render. (SDL_VIDEODRIVER=offscreen is not a substitute: it picks a
# different GL config and the frames come out wrong.)
if [[ "$(uname -s)" == "Linux" && "${GOLDEN_HEADLESS:-1}" != "0" &&
      -z "${GOLDEN_IN_XVFB:-}" ]]; then
    command -v xvfb-run >/dev/null || {
        echo "golden: xvfb-run not found; install it (Arch:" \
             "xorg-server-xvfb, Debian: xvfb) or set GOLDEN_HEADLESS=0 to" \
             "render on your desktop" >&2
        exit 2
    }
    export GOLDEN_IN_XVFB=1
    # Xvfb only owns an X display, and SDL2 prefers its wayland backend when
    # WAYLAND_DISPLAY is set — it would open the window on the real compositor
    # and ignore the virtual server entirely. Pin the backend to x11 and drop
    # the wayland socket so there is nothing else for SDL to fall back to.
    export SDL_VIDEODRIVER=x11
    unset WAYLAND_DISPLAY
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

# Per-image budget. The focus highlight draws a radial gradient whose phase
# comes from the wall clock (borealis animation.cpp, updateHighlightAnimation),
# so the highlight border differs between any two runs — the wider the focused
# row, the longer that border and the more pixels drift. The torrent-selection
# rows are the widest in the app and land at 15-22k against a 25000 default,
# close enough that an unlucky phase would fail the run for no reason. They get
# their own ceiling instead of pushing everyone else's up. (Fixing the cause
# would mean patching borealis, which is a pinned submodule.)
budget_for() {
    case "$1" in
        torrent-selection-*|ru-torrent-selection-*|update-chooser-*|ru-update-chooser-*) echo 40000 ;;
        *) echo "$MAX_DIFF" ;;
    esac
}

# render_and_compare <name> <screen> <theme> <locale>
render_and_compare() {
    local name="$1" screen="$2" theme="$3" locale="$4"
    local current="$OUT_DIR/$name.png"
    local golden="$GOLDEN_DIR/$name.png"
    local budget
    budget="$(budget_for "$name")"

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
    elif [[ "${ae%%.*}" -gt "$budget" ]]; then
        echo "FAIL  $name: $ae px differ (budget $budget, fuzz $FUZZ)"
        fail=1
    else
        echo "ok    $name: $ae px within budget $budget"
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
