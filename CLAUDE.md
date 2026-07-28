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

Run-to-run noise is real, and the `torrent-selection` screens carry their own
budget because of it. Triaging a failure, re-rendering one screen instead of
all 38, and deciding whether to re-baseline are covered by the `golden` skill
(`.claude/skills/golden/`) — invoke it before reading a diff image.

## LSP

Use clangd for C/C++ work in `src/` and `tests/` — it catches C++20-only
constructs leaking into the shared C++17 code (see above) before a build does.
`.clangd` routes `src/ui/**` to `build-golden`'s compile database (C++20,
borealis includes) and everything else to the root one, generated from
`Makefile.pc` (C++17 — the stricter build). Both are gitignored; regenerate
after touching source-file lists or flags:

```
make -f Makefile.pc clean && bear -- make -f Makefile.pc test
cmake -S . -B build-golden -DPIPENSX_GOLDEN=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

It is also how you navigate the big files without reading them whole:
`documentSymbol` first, then `Read` with `offset`/`limit` at the line it
names. `src/ui/catalog/catalog_view.hpp` is 78 KB; its symbol map is under a
tenth of that. `goToDefinition`, `findReferences` and `incomingCalls` resolve
through the real AST, so they follow what grep cannot.

## libnx and Switch-only code

Covered by the `libnx` skill (`.claude/skills/libnx/`) — invoke it before
touching a libnx call, the PC shim, or the install backend. It carries the
rules that cost a build to rediscover.

## Conventions

- `docs/plans/` is **historical**, not a roadmap: it documents design work and
  may describe code that has since changed or been removed (see its README).
  Source, tests, `README.md` and `BUILD.md` are authoritative.
- Search `src/` and `tests/` (244 files), not the tree. `vendor/` is 5525
  files of pinned third-party code: a domain term barely touches it, but a
  generic C identifier drowns in it — `malloc` is 7 hits in our code and 376
  in vendor. Reach into `vendor/` only when the task names it, and then name
  the subdirectory.
- Git: branch off `main`, then `git merge --ff-only` back and push — history
  stays linear, no merge commits.
- CI on push and PR: `ci` (make test + gitleaks history scan) and `golden`.
  Both must be green.
