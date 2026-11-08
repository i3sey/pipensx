# Agent workflow

## Work from the checkout

Use `git status --short` and `git branch --show-current` to establish the starting
state. Branch new work from `main`; continue an existing task on its current
branch. If unrelated work prevents switching safely, use a separate worktree
from `main`. Do not discard edits or move another task's branch.

Read only the feature docs needed for the change. Prefer source and tests over
claims in old plans. Use `rg -n PATTERN src/<area> tests/` for behavior and
`rg --files <area>` for discovery. Hidden agent configuration needs an explicit
path or `--hidden`; ignored dependencies need `rg --no-ignore` with a specific
path. Limit output rather than dumping large files or entire directories.

## Validate the change

For a fast iteration, build and run the affected native test:

```sh
make -f Makefile.pc build-pc/tests/test_piece
./build-pc/tests/test_piece
```

Replace `test_piece` with the relevant test. Native test filenames are discovered
automatically, but each new binary still needs a link rule in `Makefile.pc`.
Focused runs do not replace the checks required by `AGENTS.md`. Run the full
suite once the change is ready; repeat it when subsequent changes warrant it.
Do not run concurrent builds that write into the same build directory.

`test_manager` needs live network traffic. If it fails, distinguish a failed
assertion from missing network access; report the actual result. Do not weaken
assertions or claim that a build proves runtime or physical Switch behavior.
If a required tool, dependency, network service, or console is unavailable,
complete independent checks and report the blocked command and reason.

For documentation-only changes, verify local link targets and run
`git diff --check`. No application rebuild is needed.

## Clangd database

`.clangd` reads the PC compilation database from `build-pc/`. After changing
source lists or compiler flags, capture a clean build from the repository root:

```sh
make -f Makefile.pc clean
mkdir -p build-pc
bear --output build-pc/compile_commands.json -- make -f Makefile.pc test
```

This provides PC flags for shared code; it does not validate Switch-only APIs.
Do not commit the generated database. If Bear is unavailable, report that and
continue with source searches and the required builds.

## Finish

Review `git diff --stat`, the changed hunks, and `git diff --check`. Summarize
what changed, commands actually run, and any remaining limitations. Distinguish
checks that passed from checks skipped or blocked. Keep generated files and
unrelated edits out of the change. Publication and issue/PR writes follow
[issue-tracker.md](issue-tracker.md) and the user's authorization.
