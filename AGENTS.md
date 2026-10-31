# pipensx

Native Nintendo Switch-homebrew BitTorrent download manager and streaming
package installer. C11/C++ with borealis UI and vendored libutp, zstd and dht.

## Build and compatibility

```sh
make switch  # aarch64 NRO, CMake
make test    # PC assert-based test suite, Makefile.pc
make pc      # portable CLI, Makefile.pc
```

`CORE_SOURCES`, `APP_SERVICE_SOURCES`, and `UI_SOURCES` build in Switch
`pipensx`. `Makefile.pc` also compiles `src/app/*.cpp` for unit tests.
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
- A task is not done until its checks pass: run `make -f Makefile.pc test` for
  shared/core changes, and the affected build (`make switch` for Switch code,
  `make pc` for the portable CLI) — never only the one you edited in.
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
  `pipensx.nro`, `build-switch/**`, anything under `bug-reports/`.
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
 ```
 PC objects and test binaries go under `build-pc/` (never next to `src/` /
 `tests/` sources).
- Branch from `main`; merge with `--ff-only`. CI requires `make test`, GitLab Secret Detection and SAST.

## Agent skills

### Physical Switch testing

For visual checks or remote navigation on the user's Switch, read
[switch-visual-test](.agents/skills/switch-visual-test/SKILL.md).
It includes a Python client for sys-botbase screenshots and controller input.
Agents need image perception to inspect captured screens. This skill is
project-local and can be used by any harness that reads this file.

### Issue tracker

Issues live in GitHub Issues for this repo; use the `gh` CLI for all operations. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles map to `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.

<!-- antislop:start -->
## antislop
For UI, copy, people, mobile layout, or code comments work, read `antislop.md` (core) and then the skill for the task:
- UI / visual: `.agents/skills/antislop-ui/SKILL.md`
- Copy & text: `.agents/skills/antislop-copywriting/SKILL.md`
- People: `.agents/skills/antislop-human/SKILL.md`
- Mobile / responsive: `.agents/skills/antislop-layoutmobile/SKILL.md`
- Code comments: `.agents/skills/antislop-code/SKILL.md`
Before starting, follow the core's "Two Usage Modes" section in strict order: explicit session instruction first, then global preference, then ask. A session instruction always wins. For a resolved mode, say `antislop active: <mode> (session override).` or `antislop active: <mode> (global preference).` once before presenting findings or making edits, using the actual mode and source. Acknowledging the user's request without naming the source does not replace this notice.
Only an explicit choice of antislop during or after selects a session mode. A request to review, audit, or avoid file edits does not select a mode; read the global preference in that case. Another skill's mode does not select antislop's mode.
If the mode is unresolved, ask during/after and end the response; wait for the answer before any UI review, planning, or concept. For read-only tasks, put the active-mode notice only at the start of the final answer, never in progress messages. For editing tasks, announce before the first edit and omit it from the final answer.
To update antislop later: download `antislop.md` again, or run `npx antislop-ai --update` if it was installed as skill folders.
<!-- antislop:end -->

