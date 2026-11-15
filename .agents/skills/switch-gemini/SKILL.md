---
name: switch-gemini
description: Delegate physical Nintendo Switch testing and console interaction to Gemini 3.8 through local agyx. Use for deployment, screenshots, controller input, console files and logs, and runtime checks in pipensx.
---

# Delegate Switch work to Gemini

The user's standing preference is to spend Codex tokens on implementation and
reasoning, while Gemini performs console interaction. Delegate the entire
physical-console scenario, including inspecting screenshots, to `agyx` using
`gemini-3.8-flash-high`. Host-only unit tests and code changes remain with Codex;
Switch builds needed for deployment may be included in the delegated task.

## Explicit permission before console work

Before each console work session, ask the user explicitly for permission and
wait for an affirmative answer. State the concrete scenario and intended actions
(screenshots/navigation, upload/launch, or other changes). Explain that this
confirmation is required by the user's standing instruction in this skill.
Prepare code, builds and the handoff first so the user approves a concrete task.
Do not contact the console or start its delegated executor before approval.
An earlier session's approval, a general request to test, and agyx auto-approval
do not replace this confirmation. One approval covers the stated scenario and
its ordinary retries; ask again for a new session or additional actions outside
that scope. Include the user's approval and scope in the handoff.

## Handoff

Create a task directory with `mktemp -d /tmp/pipensx-switch-gemini.XXXXXX`.
Write a self-contained `task.txt` there containing:

- Repository absolute path, current branch, relevant changes and build status.
- The requested scenario and observable pass/fail checkpoints.
- Which actions the user authorized, including upload, launch, termination,
  downloads or installations when relevant. CLI auto-approval does not expand
  the user's task scope.
- An instruction to read root `AGENTS.md` and
  `.agents/skills/switch-visual-test/SKILL.md`; read the libnx skill only if the
  task needs its APIs or build guidance. Root instructions give the Docker
  command for Switch builds; do not use host `make bot-run` to build.
- An instruction that Gemini is the console executor: perform the task directly
  and do not delegate it back through this skill or recursively invoke agyx.
- An instruction to leave source files unchanged, preserve existing work, avoid
  concurrent builds, keep all screenshots and detailed logs in the task
  directory, and detach the virtual controller on exit, including failures.
- An instruction to write `report.md` in that directory with PASS/FAIL/BLOCKED
  per checkpoint, observations, artifact paths, commands/build results,
  remaining gaps, and confirmation of controller detachment. A build alone
  does not establish console behavior. End stdout with a short summary.
- An instruction to update the screenshot gallery as described below and
  include its verified URL in the report.

## Current screenshot gallery

Have Gemini publish actual screenshots from each approved console session to
the existing local gallery at `http://192.168.1.148:8766/`. Its current document
root is `/home/i3sey/.local/share/pipensx-screens`, served by Python's
`http.server` on port 8766. This is a host gallery, separate from the console IP.
Reuse the existing server and page layout; inspect the listener before changing
anything and do not kill an unrelated process or start a competing server.
If unavailable, report the gallery blocker rather than claim publication.

Keep raw evidence in the task directory and copy selected screenshots into the
gallery under unique session-specific names. Update the index with the latest
session first, the capture date/time and timezone, the tested build/commit and
whether it included uncommitted changes, and short Russian captions describing
what was actually observed. Preserve older evidence, label historical/before
images explicitly, and distinguish failed checkpoints from passing ones.
Use unique image URLs to avoid stale browser caches. Verify the gallery page
and each newly linked image through HTTP, then return the clickable gallery URL
and evidence paths. Codex reads that report without reopening the images.

Run from the repository root, substituting the actual task directory:

```bash
agyx --model gemini-3.8-flash-high --output-format text \
  --print "$(cat "$task_dir/task.txt")" \
  >"$task_dir/agent.log" 2>&1
```

`agyx` is installed at `/home/i3sey/.local/bin/agyx` and wraps
`agy --dangerously-skip-permissions`. Use the executable directly; interactive
shell alias expansion is unnecessary. If unavailable, inspect `command -v agyx`
and `agy --help`. Do not install tools or silently switch providers/models.
Use `agy models` if the specified model becomes unavailable and ask the user
to choose a replacement.

## Keep the parent context small

Use asynchronous command execution and bounded waits; redirect verbose output
to `agent.log` rather than streaming it into Codex. Read `report.md` on completion.
Only inspect a short filtered log excerpt when the report is missing or the
process fails. Do not open console screenshots, navigate, poll console state,
or fetch console logs in Codex as a routine verification step.

If findings require a fix, implement it in Codex, then give Gemini a focused
retest task with the previous report path and new build status. Stop after
three unsuccessful attempts at the same checkpoint. If Gemini is unavailable
or cannot inspect images, report the blocker and ask before taking over
console interaction yourself. Report checks actually observed, including
failures and gaps; never present delegated work as independently verified.
