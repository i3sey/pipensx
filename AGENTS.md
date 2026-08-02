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
- For UI, theme, Borealis-widget, or locale changes, load the `golden` skill
  from `.agents/skills/golden/`. Never re-baseline an unexplained diff. Run
  full `make golden` before considering a UI change done.

## Switch code

Before changing a libnx call, PC shim, install backend, or code under
`src/install`, `src/platform`, or `src/main_switch.cpp`, load the `libnx` skill
from `.agents/skills/libnx/`. It documents header-first API checks, the shared
PC-build patterns, and required dual-build verification.

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
- Branch from `main`; merge with `--ff-only`. CI requires `make test`, gitleaks,
  and golden checks.
