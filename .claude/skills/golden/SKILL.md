---
name: golden
description: >
  How to run and triage the golden-screenshot harness in this repo. Use when
  scripts/golden.sh or `make golden` fails, when a diff appears in
  build-golden/golden-out/diff/, when deciding whether to re-baseline
  tests/golden/, when a change touches src/ui, theme.hpp, a borealis widget,
  a screen's layout, or any user-facing string or locale file — those are the
  changes that move pixels.
---

# Golden screenshots in pipensx

Two rules carry the weight: **triage a diff with numbers, not by looking at
the PNG**, and **`update` is a decision you justify, never a way to make red
go away**.

## 1. Triage without reading the image

A 1280x720 reference is ~50-80 KB of PNG. Reading one costs far more than the
three numbers that actually tell you what happened, and reading it rarely
answers the question anyway — the interesting difference is usually a few
hundred pixels somewhere you would not notice by eye.

`scripts/golden.sh check` already prints the first number (AE, the count of
differing pixels). Get the other two from the pair it compared:

```bash
triage() { # <reference> <current>
    local d; d="$(mktemp /tmp/golden-triage-XXXX.png)"
    local ae; ae="$(compare -metric AE -fuzz 5% -compose Src "$1" "$2" "$d" \
                    2>&1 >/dev/null | awk '{print $1}')"
    identify -format '%@' "$d" | awk -F'[x+]' -v ae="${ae%%.*}" \
        '{a=$1*$2; printf "AE=%s bbox=%sx%s+%s+%s density=%.3f\n", \
          ae, $1, $2, $3, $4, a ? ae/a : 0}'
    rm -f "$d"
}
triage tests/golden/settings-dark.png build-golden/golden-out/settings-dark.png
```

`-compose Src` makes the diff image black except where pixels changed, so
`identify -format '%@'` returns the bounding box of the change. **Density =
AE / bbox area** is what separates the two failure modes. Measured on this
repo:

| what | AE | bbox | density |
|---|---|---|---|
| `catalog`, two identical runs | 366 | 46x19 | 0.42 |
| `torrent-selection`, two identical runs | 8276 | 1217x457 | 0.015 |
| one label moved (synthetic) | 2679 | 141x19 | 1.000 |

Read it in this order:

1. **AE against the budget.** Under it, there is no failure to explain.
2. **Large bbox, density under ~0.05** — a sparse scatter across most of the
   content area. That is the focus-highlight border drifting, the known noise
   (see §4). Not a regression.
3. **Compact bbox, density near 1.0** — a solid block of changed pixels.
   Something moved, resized, recoloured or retexted. The bbox coordinates tell
   you where on the 1280x720 screen to look in the layout code.

Only when the numbers are ambiguous, crop to the bbox and read *that* —
never the whole frame:

```bash
magick build-golden/golden-out/NAME.png -crop 141x19+420+300 +repage /tmp/c.png
```

## 2. Re-render only the screen you touched

A full `check` renders every screen x theme plus the Russian pass plus the
behaviour checks. Scoping it to one screen takes ~4s instead:

```bash
GOLDEN_SCREENS=settings GOLDEN_THEMES=dark \
GOLDEN_RU_SCREENS=' ' GOLDEN_BEHAVIOR_SCREENS=' ' scripts/golden.sh check
```

**The spaces are load-bearing.** `golden.sh` reads these as
`${GOLDEN_RU_SCREENS:-<default>}`, and `:-` substitutes the default when the
variable is empty *as well as* when it is unset — so `GOLDEN_RU_SCREENS=`
silently runs the full Russian pass anyway. A single space is non-empty, and
the loop over it iterates zero times.

Scoping is for the iteration loop only. Run the unscoped `make golden` before
calling a UI change done: a shared widget or a theme value moves screens you
did not think you touched.

## 3. Behaviour screens are not screenshots

`GOLDEN_BEHAVIOR_SCREENS` (`downloads-back`, `torrent-selection-scroll`,
`hints-budget`, `hints-budget:ru`, `bug-report-focus`) assert inside the
runner and exit non-zero. They are never compared against `tests/golden/`,
so there is no diff to triage and no baseline to update — the failure detail
is in `build-golden/golden-out/<name>-behavior.log`. Read that log, not the
PNG. A behaviour failure is always a real regression.

## 4. The noise is real, and it has one cause

The borealis focus highlight draws a radial gradient whose phase comes from
the wall clock (`updateHighlightAnimation` in the pinned borealis submodule),
so the highlight border differs between any two runs — the wider the focused
row, the longer the border and the more pixels drift. This is why
`torrent-selection` scores 8k px against itself while `catalog` scores 366.

Hence `budget_for()` in `scripts/golden.sh:104`: `torrent-selection-*` and
`ru-torrent-selection-*` get 40000, everything else the 25000 default. If a
screen starts flirting with its ceiling, give it its own case there rather
than raising `GOLDEN_MAX_DIFF` for all 38 images.

## 5. When `update` is right

Re-baseline when you *intended* the pixels to change and the triage confirms
the change is the one you made: a layout you edited, a string you rewrote, a
theme value you tuned. Say what moved and why before running it.

```bash
scripts/golden.sh update      # rewrites every reference it renders
```

`update` has no scoping guard of its own — it overwrites the baseline for
every screen in `GOLDEN_SCREENS`. Scope it the same way as §2 when only one
screen legitimately changed, so an unrelated drift does not get frozen into
the references at the same time.

Never run `update` on a diff you have not explained. A regression baked into
`tests/golden/` is invisible from then on — that is the one failure mode this
harness cannot recover from.
