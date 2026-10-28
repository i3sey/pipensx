# AutoRun compatibility preparation

`deployAutorunGame` resolves the actual EXE, prepares known dependency fixes,
seeds its AutoRun settings, and registers the game. A failed required dependency
update returns an error before the game is offered as ready to launch.

## FTL 1.6.13b (GOG)

On AutoRun `nx-amd64-fex-2640`, the original BASS pair in this release failed
while initializing `BASS.dll`, before FTL's entry point. Both FEX and Box64
failed; conservative Box64 settings alone did not resolve it. The observed
Box64 fault was at `BASS.dll+0x6404a`, followed by `loader_init` status
`0xc0000005`.

Replacing the original x86 BASS/BASSMIX pair with the current official x86
2.4 libraries allowed the game to open on the user's console (2026-10-23).
The required named imports were checked against the replacements. BASS documents
compatibility between revisions of the same API version:
https://www.un4seen.com/doc/bass/BASS_GetVersion.html

Pipensx now performs this preparation automatically when:

- The resolved EXE is `FTLGame.exe` and `ftl.dat` is beside it.
- Both audio DLLs match either the known original fingerprints or the verified
  replacements. A custom/unrecognized library set is left untouched.

The publisher URLs and exact archive/DLL SHA-256 hashes are pinned in
[src/app/autorun_service.cpp](../src/app/autorun_service.cpp).
Only the root x86 DLL member is extracted; the SDK's `x64/` files are ignored.
Every needed package and replacement is verified before any DLL is changed.

Original DLLs remain beside the replacements as `*.dll.pipensx-original`.
The complete publisher archives (including their licence documents) remain in
`.pipensx-compat/` for offline reapplication. Interrupted FAT replacements are
recovered from verified backups on the next attempt. A publisher archive change
fails checksum verification and requires updating the pinned rule.

The automatic preparation also seeds the conservative Box64 settings from the
successful hardware run. Existing user settings take precedence. No global
runtime or other game's audio libraries are changed.

## Per-game Box64 profiles

An optional `autorun.box64_conf` string in a catalog entry carries newline-
separated numeric `BOX64_DYNAREC_*` options. It survives task profile persistence
and is merged into `<actual-executable-stem>.box64.txt`, beside the resolved EXE.
Existing options and unrelated lines are preserved; repeated deployment does
not duplicate keys. Malformed options fail preparation instead of being ignored.

PC tests cover profile parsing/persistence and verified dependency transactions,
including corrupt archives/members, backup conflicts, partial downloads, cached
reapplication, and recovery after an interrupted replacement. The real official
BASS archives were also exercised through the same dependency installer locally.
