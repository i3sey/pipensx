# pipensx

Native BitTorrent download manager and streaming package installer for Nintendo
Switch homebrew. C11, borealis UI, vendored libutp/zstd/dht.

## Three builds, and shared code compiles into more than one

```
make            # help
make switch     # aarch64 NRO -> build-switch/pipensx.nro   (CMake)
make test       # PC test suite                              (Makefile.pc)
make golden     # UI screenshot + behaviour checks           (CMake, PIPENSX_GOLDEN=ON)
make pc         # portable CLI client                        (Makefile.pc)
```

`CORE_SOURCES`, `APP_SERVICE_SOURCES` and `UI_SOURCES` in `CMakeLists.txt` link
into **both** `pipensx` (real libnx) and `golden_runner` (PC, no libnx).
`Makefile.pc` compiles a third, smaller subset — including `src/app/*.cpp`. So a
change to `src/core`, `src/app` or `src/ui` can break a build you did not run;
for platform-specific code, `make switch` alone is not verification.

The CMake targets are C++20, `Makefile.pc` is **C++17**. Shared C++ under
`src/app` and `src/core` therefore has to compile as C++17: a C++20-only
construct there leaves `make switch` and `make golden` green while `make test`
fails to compile.

## Tests

`tests/test_*.c|cpp`, built and run by `Makefile.pc test`, plain `assert` — no
framework. Add a target there, not to CMake.

`test_manager` drives a real `DownloadManager`: it bootstraps DHT against live
routers and announces to a tracker, so it is slower and more
environment-sensitive than the rest. When adding a step there, wait on the
state your assertion actually depends on rather than on "something changed" —
worker status moves to `Checking` at claim time, before the torrent has been
polled even once, and several teardown behaviours are only defined after the
first poll.

## Golden screenshots

`scripts/golden.sh check` renders each screen x theme and diffs against
`tests/golden/` with a per-image pixel budget (`GOLDEN_MAX_DIFF`, default
25000). Re-baseline deliberately with `scripts/golden.sh update`, never to
silence a diff you have not explained.

It always renders on its own Xvfb display, so nothing flashes on your desktop
and stray keypresses cannot reach the runner. `GOLDEN_HEADLESS=0` to watch it.

Run-to-run noise is real and mostly the focus highlight: its radial gradient is
phased off the wall clock inside borealis, so the highlight border drifts
between any two runs, proportionally to how wide the focused row is. The
`torrent-selection` screens are the widest and get their own 40000 budget via
`budget_for()`; everything else stays on the default. Check the diff in
`build-golden/golden-out/diff/` before believing a failure.

## LSP

Use clangd for C/C++ work in `src/` and `tests/` — it catches C++20-only
constructs leaking into the shared C++17 code (see above) before a build does.
`compile_commands.json` is generated from `Makefile.pc` (matches its stricter
C++17 flags) and gitignored — regenerate after touching source-file lists or
flags:

```
make -f Makefile.pc clean && bear -- make -f Makefile.pc test
```

Not covered: `src/ui` (borealis), which only compiles under CMake/golden —
clangd falls back to generic flags there.

## libnx and Switch-only code

Covered by the `libnx` skill (`.claude/skills/libnx/`) — invoke it before
touching a libnx call, the PC shim, or the install backend. It carries the
rules that cost a build to rediscover.

## Conventions

- `docs/plans/` is **historical**, not a roadmap: it documents design work and
  may describe code that has since changed or been removed (see its README).
  Source, tests, `README.md` and `BUILD.md` are authoritative.
- Git: branch off `main`, then `git merge --ff-only` back and push — history
  stays linear, no merge commits.
- CI on push and PR: `ci` (make test + gitleaks history scan) and `golden`.
  Both must be green.
