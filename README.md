# pipensx

Native BitTorrent download manager for Nintendo Switch homebrew.

> [!NOTE]
> This project was written with substantial assistance from AI tools. AI was
> used for code generation, refactoring, testing, review, and documentation;
> generated changes are still built and validated before release.

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
- built-in Nintendo Switch catalog sourced from `bqio/switch-dumps`
- game metadata enrichment from a compact TitleDB-derived index
- magnet metadata resolution directly from RuTracker peers (BEP 9/10)
- automatic AntiZapret fallback for RuTracker tracker announces
- selectable download-only or streaming install mode
- sequential `.nsp` and `.nsz` install to SD while torrent pieces arrive
- application, update, and DLC packages; exact installed versions are skipped

Magnet links and per-file selection are not supported.

## Building

### Nintendo Switch

Requirements:

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64 and
  libnx
- CMake 3.13 or newer
- `switch-curl`, `switch-mbedtls`, `switch-zlib`, and `switch-miniupnpc`

Clone the repository together with its pinned submodules:

```bash
git clone --recurse-submodules https://github.com/i3sey/pipensx.git
cd pipensx
```

If the repository was cloned without `--recurse-submodules`, initialize the
dependencies separately:

```bash
git submodule update --init --recursive
```

Install the Switch dependencies on a devkitPro pacman installation:

```bash
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls \
  switch-zlib switch-miniupnpc
```

Build the release NRO:

```bash
export DEVKITPRO=/opt/devkitpro
make -f Makefile.switch
```

The resulting application is written to:

```text
build-switch/pipensx.nro
```

If CMake is not available through `PATH`, specify it explicitly:

```bash
CMAKE_BIN=/path/to/cmake make -f Makefile.switch
```

### PC core and tests

The portable torrent core can be built and tested on Linux or macOS with a
C/C++ toolchain, Make, pthreads, libcurl, zstd, and OpenSSL development files:

```bash
make -f Makefile.pc
make -f Makefile.pc test
```

See [BUILD.md](BUILD.md) for additional build details.

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
SD:/switch/pipensx/catalog/catalog.json
SD:/switch/pipensx/catalog/metadata/
SD:/switch/pipensx/catalog/images/
SD:/switch/pipensx/antizapret.pac
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

## Catalog

The `Catalog` tab works without a RuTracker account or access to the RuTracker
website. A catalog snapshot is bundled in the NRO. Press `R` to update it from
the latest `bqio/switch-dumps` GitHub release, `X` to filter by title, and `Y`
to change sorting.

Most catalog rows are matched to Nintendo Switch game metadata during the
build from TitleDB. Selecting a row opens a details view with canonical game
name, publisher/date/categories, description, and cached remote artwork when a
safe match exists. Rows without a match still resolve and download by their
RuTracker release data.

Selecting an entry announces its magnet info hash to the included
`bt*.t-ru.org` tracker and obtains the original torrent metadata from peers.
The metadata SHA-1 is checked before the existing torrent preview and queue
flow is opened.

## Platform Notes

- Only one torrent downloads at a time; queued tasks start automatically.
- RuTracker tracker announces connect directly first and use the supported
  AntiZapret HTTP proxy only as a fallback.
- The catalog continues using its cached or bundled snapshot when GitHub is
  unavailable.
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

Third-party components and format references are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

pipensx is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE) for the full text. Third-party components retain their
original licenses as described in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
