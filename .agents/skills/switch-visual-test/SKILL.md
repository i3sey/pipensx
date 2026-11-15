---
name: switch-visual-test
description: Test an application on a physical Nintendo Switch using sys-botbase screenshots and controller input. Use for autonomous visual checks, reproducing UI bugs, or navigating pipensx on the user's console. Requires image perception and a reachable console running the masagrator sys-botbase fork.
---

# Physical Switch visual testing & deployment

For Codex: delegate physical-console work through
[switch-gemini](../switch-gemini/SKILL.md) before executing this workflow.
For Gemini acting as the delegated console executor: follow this workflow
directly; do not invoke the delegation skill or start another agent.
Before console access, require the handoff to contain the user's explicit
approval for this session and its action scope; otherwise return BLOCKED to
Codex. After the session, update the existing screenshot gallery following the
gallery instructions in [switch-gemini](../switch-gemini/SKILL.md).

From the repository root, the client is `.agents/skills/switch-visual-test/scripts/switch_bot.py`. Use Python 3 (standard library only). Read screenshots with the available image-viewing/reading tool.

## Connection & Defaults

Default console IP is `192.168.1.143` on port `6000` (sys-botbase TCP). Can be overridden by passing `--host <IP>` or setting `SWITCH_HOST`.

```sh
# Set environment variable once (optional, 192.168.1.143 is already default):
export SWITCH_HOST=192.168.1.143
```

## Quick Commands (Fast Recipes)

```sh
# 1. One-shot build, upload and launch pipensx
make bot-run

# 2. Upload current build to console (verifies size & atomic replace)
python3 .agents/skills/switch-visual-test/scripts/switch_bot.py upload

# 3. Check what's running (title ID & bot version)
python3 .agents/skills/switch-visual-test/scripts/switch_bot.py probe

# 4. Take a screenshot
python3 .agents/skills/switch-visual-test/scripts/switch_bot.py screenshot /tmp/screen.jpg

# 5. Launch pipensx from HOME menu
python3 .agents/skills/switch-visual-test/scripts/switch_bot.py launch-pipensx

# 6. Exit/kill current foreground application back to HOME
python3 .agents/skills/switch-visual-test/scripts/switch_bot.py kill-game
```

## Tool Reference

### Navigation & Controller Input

```sh
python3 scripts/switch_bot.py probe                               # Module version & foreground Title ID
python3 scripts/switch_bot.py screenshot /tmp/screen.jpg           # Grab screen as JPEG (pixelPeek)
python3 scripts/switch_bot.py click <BUTTON>                      # A B X Y L R ZL ZR PLUS MINUS DLEFT DUP DRIGHT DDOWN HOME
python3 scripts/switch_bot.py touch <X> <Y>                       # Touch coordinates (1280x720)
python3 scripts/switch_bot.py detach                              # Detach virtual controller
```

Input commands send `getVersion` immediately afterwards as a completion barrier so input is not lost on disconnect.

### Lifecycle Management

```sh
python3 scripts/switch_bot.py kill-game                           # Close active title via killGame
python3 scripts/switch_bot.py launch-pipensx                      # Focus & launch pipensx forwarder (Title: 03DB1F658AB5E000)
```

### SD Card File Operations (sys-botbase 2.6-files-kill)

The installed sys-botbase fork supports binary file operations over TCP:

```sh
# Upload (chunked 64KB, size-verified, atomic rename from .upload)
python3 scripts/switch_bot.py upload [build-switch/pipensx.nro] [sdmc:/switch/pipensx/pipensx.nro]

# Download remote file
python3 scripts/switch_bot.py download sdmc:/switch/pipensx/settings.json /tmp/settings.json

# List directory
python3 scripts/switch_bot.py file-list sdmc:/switch/pipensx

# Check file size
python3 scripts/switch_bot.py file-size sdmc:/switch/pipensx/pipensx.nro

# Delete file
python3 scripts/switch_bot.py file-delete sdmc:/switch/pipensx/old.nro
```

## Visual Test Loop

Translate the user's requested behavior into observable checkpoints. Capture and actually inspect the initial screen, identify the current application, focused control, and any active transfer before sending input. Use one button or tap, then inspect a fresh screenshot after the UI settles. Use DPAD names `DUP`, `DDOWN`, `DLEFT`, `DRIGHT`. Touch coordinates refer to 1280×720; scale from the viewed image if resized. Stop after three unsuccessful attempts at the same transition and report the screen and attempted inputs.

Stay within the authorized scenario. Normal navigation and inspection can proceed autonomously. Downloading/installing software, deleting files or tasks, replacing application builds, terminating an active application, and interrupting transfers need authorization covering those actions. A request to test navigation alone does not authorize them. Avoid long blind input sequences and RAM writes.

For a stalled screen, capture evidence and check responsiveness before declaring a crash. Compare visible state against each checkpoint and retain before/after images outside the repository. Report observed behavior, failed checkpoints, screenshot paths, and what remains untested. **Always detach the virtual controller when done.**

## Protocol & Module Technical Notes

- Sys-botbase socket port: `6000`
- Fork version: `2.6-files-kill`
- `pixelPeek` returns hex-encoded ASCII JPEG.
- `fileWrite <path> <size>\r\n<raw_bytes>` acknowledges with `ok <size>\n` or `err <size>\n`.
- `fileRead <path> [offset] [size]\r\n` replies with `<size>\n` followed by raw binary bytes.
- `fileList <path>\r\n` streams `D <dirname>\n` and `F <size> <filename>\n` terminated by `\n`.
- `fileRename <src> <dst>\r\n` replies `ok\n` or `err\n` (Note: destination must not exist).
- `killGame\r\n` terminates active game/title.
- pipensx forwarder Title ID: `03DB1F658AB5E000` (Home screen tile #3, 3x DRIGHT from tile #0).
