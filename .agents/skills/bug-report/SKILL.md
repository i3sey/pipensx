---
name: bug-report
description: >
  Decode and triage pipensx bug reports from QR-code screenshots or photos.
  Use when the user shares a bug-report image (Settings → Report a bug), paths
  under bug-reports/, decode_report.py, QR log reconstruction, or asks to
  analyze a Switch-side crash/update/download/install failure from a reporter
  photo.
---

# Bug reports (QR log capture)

Reporters open **Settings → Help → Report a bug** (or the equivalent locale
string). The console renders the recent `pipensx.log` tail as a grid of QR
codes on one screen and sends the developer a **photo or Capture screenshot** —
nothing is uploaded automatically.

Your job: **decode → reconstruct the log → triage**. Never read a decoded log
whole; grep it by tag the way `AGENTS.md` says for `pipensx.log`.

## 1. Read the screenshot first (no decoder needed)

Before running any script, open the image with the Read tool. Extract:

| On-screen field | What it tells you |
|---|---|
| Summary line (`pipensx X · firmware Y · …`) | App version, HOS version, free space, installed count, catalog size, active downloads, **error count** |
| `Report ABCD · codes: N` | Session id `ABCD`, expected QR count |
| `(recent log only)` / `(verbose lines dropped)` | Tail was **truncated** or **filtered** before encoding — gaps are expected |
| Hint about Capture vs photo | **Detailed mode** (Y toggles): denser codes, needs a screenshot not a TV photo |
| User's filename / message | Human description of the symptom (e.g. `cant update.jpg`) |

Cross-check: decoded session id and chunk count must match the caption. A
mismatch means mixed reports or a partial photo.

## 2. Decode

Image decoding needs Pillow plus **either** `pyzbar` **or** the `zbar` package's
Python bindings (Arch: `pacman -S zbar python-pillow` — no pip needed):

```sh
# Arch
sudo pacman -S --needed zbar python-pillow

# Debian/Ubuntu (pyzbar)
sudo apt install libzbar0
python3 -m pip install pyzbar pillow
```

Run from the repo (or pass the full script path):

```sh
cd ~/pipensx
scripts/decode_report.py ~/bug-reports/cant\ update.jpg -o /tmp/report-ABCD.log
```

Stderr prints metadata, e.g.:

```text
decode_report: report B7CD (3 codes, default mode, 42112 bytes, verbose lines dropped) OK
```

**Exit codes and fixes**

| Error | Cause | Fix |
|---|---|---|
| `no QR codes in …` | Blur, glare, rotation, codes cropped | Ask for a Capture screenshot; entire grid visible |
| `missing chunk(s) [k] of N` | Photo cut off one code | Recapture whole screen; all N codes from caption |
| `CRC mismatch` | Corrupt decode (common on TV photos) | Detailed mode + Capture, or retake with less glare |
| `bad magic` | Random QR in frame (e.g. web companion QR) | Crop to bug-report screen only |
| `image decoding needs pyzbar` / `needs Pillow` | Missing deps | `pacman -S zbar python-pillow` (Arch) or `pip install pyzbar pillow` |

Raw chunk mode (no image deps, used by tests):

```sh
scripts/decode_report.py --raw chunk_0.bin chunk_1.bin …
```

Wire format: `src/app/bug_report.hpp`, must stay in lockstep with
`scripts/decode_report.py`.

## 3. Triage the reconstructed log

Work **newest → oldest**. Start with structured errors, then widen.

```sh
LOG=/tmp/report-ABCD.log

# Hard failures — always read these lines in full
rg '\[diagnostic\].*level=error' "$LOG"

# System snapshot written at report capture (version, HOS, storage, counts)
rg '\[diagnostic\].*stage=system' "$LOG"

# Symptom-specific (pick what matches the user's report)
rg '\[update\]' "$LOG"        # app self-update
rg '\[install\]' "$LOG"       # NSP/NCA install pipeline
rg '\[deploy\]' "$LOG"        # post-download extract/copy
rg '\[debrid\]' "$LOG"        # cloud download path
rg '\[magnet\]' "$LOG"        # metadata resolution
rg '\[torrent\]' "$LOG"       # peer/download engine
rg '\[dht\]' "$LOG"           # peer discovery
rg '\[storage\]' "$LOG"       # disk / FAT32 / path issues
rg '\[meta\]' "$LOG"          # torrent parse errors
rg '\[net\]' "$LOG"           # DNS, bind, socket
rg '\[catalog\]' "$LOG"       # catalog refresh/import
rg '\[installed\]' "$LOG"     # title list / NCM
```

**`[diagnostic]` lines** are the primary signal: `level=error` with
`stage=` / `tag=` / body point at the failing subsystem. **`[telemetry]`**
and borealis `[DEBUG]` lines are often **dropped** when the tail is filtered —
do not treat their absence as proof nothing happened.

**`[torrent]` / `[dht]`** are noisy; grep for `error`, `failed`, `mismatch`,
`timeout`, `rejected` before reading health spam.

Map `stage=` from diagnostics to code:

| stage | Start in |
|---|---|
| `update` | `src/ui/settings/settings_panels.hpp` (check/install/restart) |
| `install` | `src/install/install_backend_switch.cpp` |
| `deploy` | `src/app/switch_deploy.cpp` |
| `magnet` | `src/app/magnet_resolver.cpp` |
| `debrid` | `src/app/debrid_transfer.cpp` |
| `storage` | `src/platform/storage.c`, settings storage panel |
| `catalog` / `metadata` | `src/ui/catalog/`, `GameMetadataService` |
| `installed` | `src/app/installed_title_service.cpp` |
| `settings` | `src/ui/settings/` |

## 4. Report back

Use this template:

```markdown
## Bug report ABCD — <one-line symptom>

**Reporter context:** pipensx X.Y.Z · HOS A.B.C · <free space> · <user description>
**Capture quality:** <complete N/N codes | missing chunks | truncated | filtered>

### Findings
- <diagnostic error or key log line with timestamp if present>
- …

### Likely cause
<one paragraph tying log evidence to code path>

### Next steps
- [ ] <concrete fix or ask reporter for X>
```

If the log is empty or only has the system snapshot, say so — the bug may be
UI-only or happened before logging was enabled.

## 5. Constraints

- `bug-reports/` may live **outside the repo** (e.g. `~/bug-reports/`). Use
  absolute paths; the folder is cursorignored in-repo.
- Do **not** open golden PNGs or multi-MB catalog JSON while triaging.
- Changing the wire format or QR UI requires `make -f Makefile.pc test`
  (`test_bug_report`, `test_bug_report_decode.py`) and likely `make golden`
  (`bug-report`, `bug-report-detail`, `bug-report-focus` screens).
