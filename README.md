# pipensx

Native BitTorrent download manager for Nintendo Switch homebrew.

The Switch interface uses Borealis with its native NanoVG/deko3d renderer. It
follows the system UI conventions for focus, controller hints, touch input,
light/dark themes, and handheld/docked scaling.

## Features

- `.torrent` file picker for the SD card
- persistent FIFO download queue
- one active download at a time
- pause, resume, retry, and explicit recheck
- restart-safe piece verification and resume
- download details with speed, peers, DHT, and piece progress
- removal with either preserved or deleted download data
- duplicate detection by info hash
- tracker, DHT, and PEX peer discovery
- selectable download-only or streaming install mode
- sequential `.nsp` and `.nsz` install to SD while torrent pieces arrive
- application, update, and DLC packages; exact installed versions are skipped

Magnet links and per-file selection are not supported.

## Install

Copy the built NRO to:

```text
SD:/switch/pipensx/pipensx.nro
```

Launch it through hbmenu in **application mode**:

1. Close Album/hbmenu.
2. Hold `R` while launching a game.
3. Keep holding `R` until hbmenu opens.
4. Start pipensx.

Album applet mode is rejected with an instruction screen because it does not
provide enough memory and BSD network sessions for the GUI torrent client.

Application data is stored in:

```text
SD:/switch/pipensx/queue.bencode
SD:/switch/pipensx/torrents/
SD:/switch/pipensx/downloads/
SD:/switch/pipensx/dht_nodes.bin
SD:/switch/pipensx/pipensx.log
```

## Controls

- `A`: open a download or select a file
- `B`: go back
- `X`: add a torrent; on details, remove the task
- `Y`: pause, resume, retry, or recheck the selected task
- `+`: safely stop the active task and exit

The file picker also supports touch input.

For torrents containing NSP/NSZ files, the confirmation dialog offers:

- `Install to SD while downloading`: package files are reconstructed and
  committed directly to Nintendo content storage without retaining a complete
  package file.
- `Download only`: all files are stored normally under `downloads/`.

## Platform Notes

- Only one torrent downloads at a time; queued tasks start automatically.
- Package files are processed in torrent file order. Ordinary files in the
  same torrent are downloaded normally.
- A package interrupted before its commit restarts from its beginning. Packages
  already committed before the interruption remain installed and are skipped.
- Stream install supports full Application, Patch, and AddOnContent metadata.
  DeltaFragment and system-title packages are rejected.
- Existing files are checked piece by piece before a task starts or resumes.
- FAT32 cannot store an individual file larger than 4 GiB. Use exFAT for such
  torrents.
- Runtime behavior such as sleep/wake and long downloads must be validated on
  a physical Switch.

See [BUILD.md](BUILD.md) for build instructions.

Third-party components and format references are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
