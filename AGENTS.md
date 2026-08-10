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

## Debrid providers

Three download sources: TorBox (cloud), TorrServer (self-hosted LAN), and
Real-Debrid (cloud). Each implements `DebridProvider` (`src/app/debrid_provider.hpp`),
a pure-virtual interface with six required methods.

### Adding a new provider

Follow the existing pattern (`TorboxProvider` + `TorboxClient`). Touchpoints:

| Layer | Files |
|-------|-------|
| Provider interface | `src/app/debrid_provider.hpp` — add to `DebridProviderKind` enum |
| Client + provider | `src/app/<name>_client.{hpp,cpp}`, `src/app/<name>_provider.{hpp,cpp}` |
| Settings | `src/app/app_settings.hpp` — add key field; `app_settings.cpp` — serialize/deserialize field and provider kind string |
| Download manager | `download_manager.{hpp,cpp}` — `setXxxKey()`, `apiKeyFor()` branch, `makeProvider()` branch, `saveLocked`/`load` provider name |
| UI helpers | `src/ui/debrid_ui.hpp` — `activeDebridKey()`, `makeDebridProvider()`, `debridProviderName()`, `DebridLinkView` members |
| Settings UI | `src/ui/settings/settings_view.hpp` — selector list and `applyValues()` |
| First-run | `src/ui/first_run_view.hpp` — option card, diagram kind, `updateSelection()` case |
| Switch init | `src/main_switch.cpp` — call `setXxxKey()` on the manager after settings load |
| Build | `CMakeLists.txt` — add to `APP_SERVICE_SOURCES`; `Makefile.pc` — object rule, test targets, link into `test_manager` |
| Locale | `resources/i18n/*/pipensx.json` — first-run and diagram strings |
| Tests | `tests/test_<name>_client.cpp` (static parsers), `tests/test_<name>_provider.cpp` (mock transport) |

### Provider test pattern

Tests inject a `Transport` functor (e.g. `RdTransport`) that returns canned JSON
responses instead of making real HTTP calls. The provider and its client both
accept a transport in their constructors so the curl path is never hit during
testing. The transport matches requests by URL substring (endpoint name) and
returns pre-baked JSON.

### Account cleanup

When a debrid task is removed, `DownloadManager::removeFromDebridAsync` fires
a detached thread that calls `makeProvider(…)->remove(id)` to clean the account.
A detached thread is used because the caller must never block on the HTTPS
round-trip — `mutex_` may be held.

## Settings and state versioning

- **App settings** (`src/app/app_settings.cpp`): `kSettingsVersion` (currently 3).
  Adding a new stored field that requires migration needs a version bump and a
  rule in `parseSettings()`. The old field comment `"realdebrid" was a provider we
  no longer ship` was written before Real-Debrid was added — do not cargo-cult it.
- **Download state** (`src/app/download_manager.cpp`): `saveLocked` writes
  `versioni6e`. The `load()` function gates optional fields behind
  `version.ival >= N`. Adding a new persisted task field requires bumping to 7,
  adding a `version.ival >= 7` load gate, and writing the new key to `saveLocked`.

## Switch code

Before changing a libnx call, PC shim, install backend, or code under
`src/install`, `src/platform`, or `src/main_switch.cpp`, load the `libnx` skill
from `.agents/skills/libnx/`. It documents header-first API checks, the shared
PC-build patterns, and required dual-build verification.

## Context budget — don't read

- `pipensx.log` is a multi-thousand-line runtime log. Never read it whole;
  grep it by tag (`[torrent]`, `[dht]`, `[status]`, ...) when you need it.
- Do not open images, binaries, or build output: `resources/*.jpg|png`,
  `tests/golden/*.png`, `tests/fixtures/golden/*.png`, `pipensx.nro`,
  `build-golden/**`, `build-switch/**`, anything under `bug-reports/`.
  Golden diffs are triaged by the numbers (AE, bbox, density) from the
  `golden` skill — never by viewing the PNG.
- Answer symbol/flow questions through CodeGraph (`codegraph explore` or the
  `codegraph_explore` tool) before falling back to grep + Read loops.

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
