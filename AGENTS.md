# pipensx

Native Nintendo Switch homebrew: BitTorrent downloads and streaming package
installation. C11 core, C++ app services, Borealis UI.

## Start here

- Search `src/` and `tests/` for the task first. Read headers and nearby tests
  before changing behavior. Use clangd when available.
- [README.md](README.md): product behavior.
  [BUILD.md](BUILD.md): dependencies and build options.
  [docs/README.md](docs/README.md): feature docs, approved plans and historical notes.
- Source and tests establish implemented behavior. Roadmaps and dated audits
  describe proposals or snapshots; verify their claims against the code.
- Load only the skill and feature docs relevant to the task.

## Code and checks

- `src/core/`: portable torrent engine; `src/app/`: services and persisted state;
  `src/ui/`: Borealis; `src/install/` and `src/platform/`: platform boundaries.
  Browser companion assets live in `resources/web/`; `site/` is the public site.
- Switch uses C++20; shared core/app code must also compile as C11/C++17 on PC.
  Source lists live in `CMakeLists.txt` and `Makefile.pc`; tests are registered
  in `Makefile.pc`, not CMake. Objects and binaries belong in `build-pc/`.
- Shared/core/app/install changes: `make -f Makefile.pc test`.
  Switch changes: also `make switch`. CLI changes: also `make pc`.
  Documentation-only changes: check links and `git diff --check`.
- `test_manager` uses live DHT/tracker traffic and runs last. Wait for the state
  required by the assertion; claim-time `Checking` does not prove a poll occurred.
- Native locale changes: add keys to `en-US`, mirror `ru`, `pt-BR`, `fr`, and run
  `python3 scripts/check_i18n.py`. Browser strings: also run
  `python3 scripts/check_web_i18n.py`.
- After source-list or compiler-flag changes, regenerate the clangd database:
  `make -f Makefile.pc clean && bear -- make -f Makefile.pc test`.

## Task-specific instructions

| Task | Read before working |
| --- | --- |
| libnx calls, PC shims, install backend, `src/install/`, `src/platform/`, `src/main_switch.cpp` | [.agents/skills/libnx/SKILL.md](.agents/skills/libnx/SKILL.md) |
| Debrid providers or persisted settings/download-state versions | [.agents/skills/debrid/SKILL.md](.agents/skills/debrid/SKILL.md) |
| QR bug report or Switch crash/update/download/install report | [.agents/skills/bug-report/SKILL.md](.agents/skills/bug-report/SKILL.md) |
| Screenshots or controller input on a physical Switch | [.agents/skills/switch-visual-test/SKILL.md](.agents/skills/switch-visual-test/SKILL.md) |
| GitHub issues, labels or PRs | [docs/agents/issue-tracker.md](docs/agents/issue-tracker.md) |

## Keep context small

- Never read `pipensx.log` whole; filter by tag, error or task ID.
- Do not load catalog dumps, binaries, build outputs or image collections as
  text. Sample catalog fixtures only for parser work. Open individual screenshots
  or bug-report images when the task requires them, following the relevant skill.
- Search third-party trees only when needed for the named dependency/API.
- `.ignore` keeps vendor trees, catalog dumps, historical notes and mockups out
  of default ripgrep results. Use an explicit path with `rg --no-ignore` when
  the task needs one of these trees. `.cursorignore` also filters agent indexing.
- Preserve unrelated working-tree edits. Branch new work from `main`;
  merge with `--ff-only`. See [CONTRIBUTING.md](CONTRIBUTING.md) for PR conventions.

<!-- antislop:start -->
## antislop

For UI, copy, accessibility, responsive layout or code comments, read the
[core](.agents/skills/antislop/SKILL.md) and follow its Two Usage Modes section.
Then load only the relevant skill under `.agents/skills/`:
`antislop-ui`, `antislop-copywriting`, `antislop-human`,
`antislop-layoutmobile` or `antislop-code` (`SKILL.md` in each folder).
<!-- antislop:end -->
