# Contributing to pipensx

pipensx is a native BitTorrent download manager for Nintendo Switch homebrew.
Before contributing, please read [BUILD.md](BUILD.md) for build and test
instructions, and [README.md](README.md) for an overview of the application.

## Repository layout

```
src/core/      Torrent engine (C99): metainfo, pieces, peers, tracker, DHT
src/app/       Application logic (C++20): download manager, catalog, metadata
src/install/   Streaming NSP/NSZ install backends (PC + Switch)
src/platform/  Platform glue: storage, crash logging
src/main_switch.cpp  Borealis UI entry point (Switch)
src/main_pc.c        PC command-line client for engine development
tests/        C/C++ unit tests, run with `make -f Makefile.pc test`
tools/        Python helpers: catalog rebuild, metadata index, health checks
resources/    Application icon + bundled catalog snapshot
vendor/       Pinned submodules: borealis, glm, zstd, libnx-ext
```

The C core (`src/core/`) is platform-independent and is shared verbatim
between the PC and Switch builds. Keep it free of Switch- or Borealis-specific
code. Platform behavior goes in `src/platform/` or `src/install/`.

## Building and testing

Switch build (requires devkitPro):

```bash
git submodule update --init --recursive
make -f Makefile.switch
```

PC core and tests (Linux/macOS, requires libcurl, zstd, OpenSSL):

```bash
make -f Makefile.pc
make -f Makefile.pc test
```

Every change to `src/core/` or `src/install/` should keep the test suite
green. Add a test when you fix a bug or add a behavior to the engine.

## Commit conventions

Use [Conventional Commits](https://www.conventionalcommits.org/) so the
history stays readable and can drive changelogs:

```
<type>(<scope>): <subject>
```

Types: `feat`, `fix`, `perf`, `refactor`, `docs`, `build`, `test`, `chore`,
`ci`. Scope is optional but encouraged (e.g. `core`, `install`, `ui`,
`catalog`, `torrent`).

Examples:

```
feat(install): add batch queue with space checks
fix(torrent): recycle peers on choke timeout
perf(core): keep request pipeline saturated
docs: document streaming install restart behavior
```

Avoid meaningless messages (`fix`, `new`, `wip`). Keep the subject line under
72 characters, imperative mood. Put rationale and detail in the body.

## Pull requests

- One logical change per PR.
- Include tests for new behavior or bug fixes.
- Keep the Switch build working: `make -f Makefile.switch` must produce
  `build-switch/pipensx.nro`.
- Do not commit build artifacts, binaries, `.o`/`.d` files, or Python
  bytecode. They are gitignored; if a binary sneaks in, untrack it with
  `git rm --cached`.
- Runtime behavior (sleep/wake, long downloads, install) must ultimately be
  validated on physical Switch hardware; note this in the PR description when
  relevant.
