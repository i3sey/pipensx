---
name: switch-autopilot
description: Control and test a physical Nintendo Switch running sys-autopilot-plus via its native MCP endpoint and HTTP REST CLI tool. Use for screenshots, controller input, deploying pipensx builds, launching/terminating titles or NROs, reading logs, crash diagnostics, telemetry, and SD card file access.
---

# Physical Switch control & testing (`sys-autopilot-plus`)

For Codex: delegate physical-console work through
[switch-gemini](../switch-gemini/SKILL.md) before executing this workflow.
For Gemini acting as the delegated console executor: follow this workflow
directly; do not invoke the delegation skill or start another agent.
Before console access, require explicit approval in the conversation or handoff.
Session-wide approval remains valid for subsequent scenarios, retries and
continuations in the same conversation; do not request it again. Respect its
limits and any revocation, following [switch-gemini](../switch-gemini/SKILL.md).
Without approval, return BLOCKED to Codex. After the work, update the existing
screenshot gallery following [switch-gemini](../switch-gemini/SKILL.md).

## Connection & MCP

The console runs `sys-autopilot-plus` (`4200000000004150`, port `4150`) with a
native Streamable HTTP MCP endpoint configured globally in
`~/.gemini/config/mcp_config.json`:

- Endpoint: `http://192.168.1.143:4150/mcp`
- REST base: `http://192.168.1.143:4150` (override with `SWITCH_HOST`,
  `SWITCH_PORT`, or `--host` / `--port`; optional `SWITCH_TOKEN` if auth is
  enabled in `sdmc:/config/sys-autopilot/config.ini`)
- pipensx forwarder Title ID: `03DB1F658AB5E000`
- pipensx NRO path: `/switch/pipensx/pipensx.nro`

## MCP vs HTTP REST CLI (When to Use Which)

Both channels talk to the same `sys-autopilot-plus` server on port `4150`. Pick
the right transport for the operation:

- **Use native MCP tools (`switch` server)** for interactive work:
  - `screenshot` (returns the image directly to the model without temp files)
  - `tap_buttons`, `tap_sequence`, `touch_screen`, `set_stick`, `controller_handshake`, `clear_input`
  - `status`, `get_running_process`, `launch_title`, `launch_nro`, `terminate_process`
  - `get_telemetry`, `get_last_crash_report`, `list_installed_titles`
  - `list_directory`, `read_file` (with negative `offset` to tail logs), `delete_file`, `hash_file`, memory tools
- **Use the HTTP REST CLI (`switch_autopilot.py`) or `curl -T`** for binary transfers and disk artifacts:
  - **Uploading `.nro` builds, ROMs, or any binary/large file** (`PUT /files`): streams raw bytes directly over HTTP (atomic `.tmp` rename + SHA-256 verification). **Never** pass binaries or multi-KB files through MCP `upload_file`, because MCP arguments are base64-encoded token-by-token through the model context.
  - **Streaming NSP/XCI installs** (`PUT /install`): streams straight to NCM with no SD staging.
  - **Saving screenshots to disk** (`GET /screenshot`) for task artifacts and the local screenshot gallery.
  - **Scripted / non-MCP sessions** or quick `tail` / `download` of console files.

## CLI Quick Commands

```sh
CLI=.agents/skills/switch-autopilot/scripts/switch_autopilot.py

# Check server status & foreground process
python3 "$CLI" probe

# Upload pipensx.nro (atomic PUT + SHA-256 verification)
python3 "$CLI" upload

# Launch pipensx forwarder (03DB1F658AB5E000) directly
python3 "$CLI" launch-pipensx

# Launch arbitrary title or NRO / terminate foreground process
python3 "$CLI" launch-title 03DB1F658AB5E000
python3 "$CLI" launch-nro /switch/pipensx/pipensx.nro
python3 "$CLI" terminate

# Save JPEG screenshot (for gallery / task artifacts)
python3 "$CLI" screenshot /tmp/screen.jpg

# Controller input
python3 "$CLI" tap A
python3 "$CLI" tap DDOWN --duration-ms 120
python3 "$CLI" touch 640 360
python3 "$CLI" stick left 0.0 -1.0 --duration-ms 300
python3 "$CLI" handshake
python3 "$CLI" detach

# SD card files & logs
python3 "$CLI" ls /switch/pipensx
python3 "$CLI" tail /switch/pipensx/pipensx.log 4096
python3 "$CLI" download /switch/pipensx/settings.json /tmp/settings.json
python3 "$CLI" hash /switch/pipensx/pipensx.nro
python3 "$CLI" rm /switch/pipensx/old.nro

# Telemetry, crash reports, installed titles, streamed NSP/XCI install
python3 "$CLI" telemetry
python3 "$CLI" crash
python3 "$CLI" titles
python3 "$CLI" install /path/to/pkg.nsp --storage sd

# Power (requires explicit authorization)
python3 "$CLI" restart
python3 "$CLI" sleep
```

## Deploy & Relaunch (`pipensx.nro`)

- **Terminate before uploading**: if `probe` shows `03db1f658ab5e000` is running,
  run `terminate` first and wait ~1s before `upload`. Uploading while `pipensx`
  is open transfers all bytes and then fails on rename with HTTP 500
  `failed to move uploaded file into place`.
- **Dismiss the post-terminate modal**: `terminate` causes HOS to show
  *"The software was closed because an error occurred."* (`OK`). Dismiss it with
  `tap A` (and wait ~1–2s) before launching anything.
- **Launching after upload**: `launch-pipensx` (`pmshellLaunchProgram`) may fail
  with `0xba800a08` / `0xbac00a08` while the error modal is visible or on HOME.
  Right after `terminate` + `tap A`, `pipensx` is already focused as the first
  tile on HOME — verify via `screenshot` and launch it with `tap A` (handling a
  profile picker if one appears), then verify `probe` shows `03db1f658ab5e000`
  running and call `detach`.

## Visual Test Loop

Translate the user's requested behavior into observable checkpoints. Capture and
inspect the initial screen, identify the active application, focused control,
and any active transfer before sending input. Prefer single inputs or short
verified steps, then inspect a fresh screenshot after the UI settles. Touch
coordinates use `1280x720`. Stop after three unsuccessful attempts at the same
transition and report the screen and attempted inputs.

Stay within the authorized scenario. Normal navigation and inspection can
proceed autonomously. Downloading/installing software, deleting files or tasks,
replacing application builds, terminating an active application, power actions,
and interrupting transfers need authorization covering those actions. Avoid long
blind input sequences and unrequested RAM pokes.

For a stalled screen, check `get_telemetry` / `get_running_process` and
`get_last_crash_report` before declaring a crash. Retain before/after evidence
outside the repository. Report observed behavior, failed checkpoints,
screenshot paths, and what remains untested. **Always detach the virtual
controller when done** (`python3 "$CLI" detach` or `POST /controller/detach`).
