# pipensx

Native Nintendo Switch homebrew: BitTorrent downloads and streaming package
installation. C11 core, C++ app services, Borealis UI.

## Start here

- Check `git status --short` and the current branch before editing. Preserve
  existing changes; do not reset, stash, or overwrite them to simplify the task.
- For behavior changes, search `src/` and `tests/` first. Read headers and nearby
  tests before editing. For tooling/docs work, start with the relevant config.
  Use clangd when available; use scoped `rg` searches otherwise.
- [README.md](README.md): product behavior.
  [BUILD.md](BUILD.md): dependencies and build options.
  [docs/README.md](docs/README.md): feature docs, approved plans and historical notes.
- Source and tests establish implemented behavior. Roadmaps and dated audits
  describe proposals or snapshots; verify their claims against the code.
- Load only the skill and feature docs relevant to the task. Do not read every
  skill, roadmap, or dependency tree at startup.
- See [agent workflow](docs/agents/workflow.md) for focused checks, clangd setup,
  and completion reporting.

## Code and checks

- `src/core/`: portable torrent engine; `src/app/`: services and persisted state;
  `src/ui/`: Borealis; `src/install/` and `src/platform/`: platform boundaries.
  Browser companion assets live in `resources/web/`; `site/` is the public site.
- Borealis UI tabs and sub-pages must be created lazily on first selection — do
  not add startup warmup timers that pre-construct hidden views. Defer SD/network
  loads and repeating timers to `willAppear()` (not view constructors) and stop
  timers in `willDisappear()`.
- Switch uses C++20; shared core/app code must also compile as C11/C++17 on PC.
  Source lists live in `CMakeLists.txt` and `Makefile.pc`; native tests are discovered as
  `tests/test_*.c` and `tests/test_*.cpp`, with link rules in `Makefile.pc`. Objects and binaries belong in `build-pc/`.
- Shared/core/app/install changes: `make -f Makefile.pc test`.
  Switch changes: also `make switch`. CLI changes: also `make pc`.
  Documentation-only changes: check links and `git diff --check`.
- On this workstation, devkitPro is installed in the local Docker image
  `devkitpro/devkita64:zstd`, at `/opt/devkitpro` inside the container; it is
  not installed on the host. Build from the repository root with:
  ```sh
  docker run --rm --pull=never --user "$(id -u):$(id -g)" \
    -e DEVKITPRO=/opt/devkitpro -v "$PWD:$PWD" -w "$PWD" \
    devkitpro/devkita64:zstd make switch
  ```
  Then upload and launch from the host using `switch_autopilot.py` (see the
  `switch-autopilot` skill). `make bot-run` on the host attempts a host build.
- `test_manager` uses live DHT/tracker traffic and runs last. Wait for the state
  required by the assertion; claim-time `Checking` does not prove a poll occurred.
- Native locale changes: add keys to `en-US`, mirror `ru`, `pt-BR`, `fr`, and run
  `python3 scripts/check_i18n.py`. Browser strings: also run
  `python3 scripts/check_web_i18n.py`.
- After source-list or compiler-flag changes, regenerate the clangd database
  using the [workflow commands](docs/agents/workflow.md#clangd-database).
  `.clangd` expects `build-pc/compile_commands.json`.

## Task-specific instructions

| Task | Read before working |
| --- | --- |
| libnx calls, PC shims, install backend, `src/install/`, `src/platform/`, `src/main_switch.cpp` | [.agents/skills/libnx/SKILL.md](.agents/skills/libnx/SKILL.md) |
| Debrid providers or persisted settings/download-state versions | [.agents/skills/debrid/SKILL.md](.agents/skills/debrid/SKILL.md) |
| QR bug report or Switch crash/update/download/install report | [.agents/skills/bug-report/SKILL.md](.agents/skills/bug-report/SKILL.md) |
| Physical Switch tests, deployment, screenshots, input or console files/logs | [.agents/skills/switch-gemini/SKILL.md](.agents/skills/switch-gemini/SKILL.md): Codex delegates to Gemini through `agyx`; Gemini uses [switch-autopilot](.agents/skills/switch-autopilot/SKILL.md) (and the `switch` MCP server) directly |
| GitHub issues, labels or PRs | [docs/agents/issue-tracker.md](docs/agents/issue-tracker.md) |

## Keep context small

- Ask for Switch access once per conversation session, unless already explicitly
  approved. Approval covers subsequent task scenarios, retries and continuations
  in that conversation; honor any limits or revocation. See
  [switch-gemini](.agents/skills/switch-gemini/SKILL.md). The authorized executor
  performs console work and updates the current screenshot gallery at
  <http://192.168.1.148:8766/>.

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

For UI, product copy, accessibility, responsive layout or code-comment work, read the
[core](.agents/skills/antislop/SKILL.md) and follow its Two Usage Modes section.
Then load only the relevant skill under `.agents/skills/`:
`antislop-ui`, `antislop-copywriting`, `antislop-human`,
`antislop-layoutmobile` or `antislop-code` (`SKILL.md` in each folder).
Technical instructions and build/config maintenance do not require design or
copywriting skills unless the task also changes product copy or UI.
<!-- antislop:end -->
