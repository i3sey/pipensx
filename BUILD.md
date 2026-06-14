# Build Instructions

Initialize the pinned dependencies after cloning:

```bash
git submodule update --init --recursive
```

## Nintendo Switch

Requirements:

- devkitA64 and libnx
- CMake 3.13 or newer
- `switch-curl`
- `switch-mbedtls`
- `switch-zlib`
- `switch-miniupnpc`

On a devkitPro pacman installation:

```bash
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls \
  switch-zlib switch-miniupnpc
```

Build:

```bash
export DEVKITPRO=/opt/devkitpro
make -f Makefile.switch
```

Artifact:

```text
build-switch/pipensx.nro
```

Run the NRO through hbmenu title override. Album applet mode is intentionally
rejected before Borealis/deko3d initialization.

The build embeds the pinned Borealis UI library, zstd decompressor, libnx-ext
IPC helpers, GLM headers, deko3d shaders, system-status images, Material icon
font, application icon, and NACP metadata.

Use a non-default CMake executable when required:

```bash
CMAKE_BIN=/path/to/cmake ./scripts/build_switch.sh
```

## PC Core And Tests

Requirements: GCC/G++, Make, pthreads, libcurl, zstd, and OpenSSL development
files.

```bash
make -f Makefile.pc
make -f Makefile.pc test
```

The PC command-line client remains available for torrent-engine development:

```bash
./pipensx /path/to/file.torrent [output_dir]
```

The test suite covers piece verification, disk-corruption recovery, restart
resume, metainfo path safety, queue persistence, duplicate import, pause/resume,
task/data removal, incremental PFS0 parsing, and real zstd NCZ reconstruction.
