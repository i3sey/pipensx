# pipensx

Native Nintendo Switch-homebrew BitTorrent download manager and streaming
package installer. C11/C++ with borealis UI and vendored libutp, zstd and dht.

## Build and compatibility

```sh
make switch  # aarch64 NRO, CMake
make test    # PC assert-based test suite, Makefile.pc
make golden  # PC UI screenshots and behaviour checks, CMake
make pc      # portable CLI, Makefile.pc
```

`CORE_SOURCES`, `APP_SERVICE_SOURCES`, and `UI_SOURCES` build in both Switch
`pipensx` and PC `golden_runner`. `Makefile.pc` also compiles `src/app/*.cpp`.
CMake uses C++20 but shared `src/core` and `src/app` code must compile as
C++17. Verify every affected build; a Switch build alone is insufficient for
platform-specific changes.

## Tests and UI

- Tests are `tests/test_*.c|cpp`, plain `assert`, registered in `Makefile.pc`,
  not CMake.
- `test_manager` uses live DHT and a tracker. Wait for the state an assertion
  needs; claim-time `Checking` does not mean the torrent was polled.
  `test_manager` runs last in the suite — it boots DHT against live routers, and
  ordering it last keeps a bad network from hiding the other tests.
- For UI, theme, Borealis-widget, or locale changes, load the `golden` skill
  from `.agents/skills/golden/`. Never re-baseline an unexplained diff. Run
  full `make golden` before considering a UI change done.
- A task is not done until its checks pass: run `make -f Makefile.pc test` for
  shared/core changes, `make golden` for UI changes, and the affected build
  (`make switch` for Switch code, `make pc` for the portable CLI) — never only
  the one you edited in.
- Adding or changing locale strings must pass `scripts/check_i18n.py`. It
  validates placeholder counts across all supported languages. Add new keys to
  `en-US` first (the fallback), then mirror to `ru`, `pt-BR`, and `fr`.

When adding a debrid provider or changing persisted settings / download-state
versioning, load the `debrid` skill from `.agents/skills/debrid/`.

## Switch code

Before changing a libnx call, PC shim, install backend, or code under
`src/install`, `src/platform`, or `src/main_switch.cpp`, load the `libnx` skill
from `.agents/skills/libnx/`. It documents header-first API checks, the shared
PC-build patterns, and required dual-build verification.

When triaging a user bug report from a QR screenshot or photo, load the
`bug-report` skill from `.agents/skills/bug-report/`. It covers
`scripts/decode_report.py`, log grep patterns, and the triage output format.

## Context budget — don't read

- `pipensx.log` is a multi-thousand-line runtime log. Never read it whole;
  grep it by tag (`[torrent]`, `[dht]`, `[status]`, ...) when you need it.
- Do not open images, binaries, or build output: `resources/*.jpg|png`,
  `tests/golden/*.png`, `tests/fixtures/golden/*.png`, `pipensx.nro`,
  `build-golden/**`, `build-switch/**`, anything under `bug-reports/`.
  Golden diffs are triaged by the numbers (AE, bbox, density) from the
  `golden` skill — never by viewing the PNG.
- Do not read `resources/catalog/*.json` (tens of MB). Grep or sample if a
  catalog-parser change needs a fixture.

## Conventions

- `docs/plans/` is historical. Source, tests, `README.md`, and `BUILD.md` are
  authoritative.
- Search `src/` and `tests/` first; do not search `vendor/` unless the task
  names the relevant vendored component.
- Use clangd for C/C++ navigation when available. Regenerate compile databases
  after changing source lists or flags:
```sh
 make -f Makefile.pc clean && bear -- make -f Makefile.pc test
 cmake -S . -B build-golden -DPIPENSX_GOLDEN=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
 ```
 PC objects and test binaries go under `build-pc/` (never next to `src/` /
 `tests/` sources).
- Branch from `main`; merge with `--ff-only`. CI requires `make test`, gitleaks,
  and golden checks.

## Agent skills

### Issue tracker

Issues live in GitHub Issues for this repo; use the `gh` CLI for all operations. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles map to `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.
