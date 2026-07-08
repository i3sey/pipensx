# Build instructions

## Source checkout

Clone with the pinned submodules, or initialize them in an existing checkout:

```bash
git clone --recurse-submodules https://github.com/i3sey/pipensx.git
cd pipensx
# Existing clone only:
git submodule update --init --recursive
```

Run `make` to list the supported top-level targets.

## PC client and tests

On Debian or Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential python3 libcurl4-openssl-dev libssl-dev \
  libzstd-dev zlib1g-dev
```

Build the portable command-line client or run the complete test suite:

```bash
make pc
make test
```

The client is written to `./pipensx` and accepts:

```bash
./pipensx /path/to/file.torrent [output_dir]
```

## Nintendo Switch

Requirements:

- devkitPro with devkitA64 and libnx
- CMake 3.13 or newer
- `switch-curl`, `switch-mbedtls`, `switch-zlib`, and `switch-miniupnpc`

Install them with devkitPro pacman:

```bash
sudo dkp-pacman -S switch-dev switch-curl switch-mbedtls \
  switch-zlib switch-miniupnpc
```

Build the release NRO:

```bash
export DEVKITPRO=/opt/devkitpro
make switch
```

The artifact is `build-switch/pipensx.nro`. Override CMake when it is not on
`PATH`:

```bash
make switch CMAKE_BIN=/path/to/cmake
```

Public builds contain no catalog or game metadata dataset. A builder who has a
lawfully obtained compatible metadata index may embed it explicitly:

```bash
make switch PIPENSX_METADATA_INDEX=/absolute/path/game_metadata_index.json
```

The path is build input only. Do not copy or commit the dataset under
`resources/catalog/`; that directory is ignored deliberately.

## Golden screenshot tests

Golden tests require CMake, Ninja, SDL2, ImageMagick, Xvfb, Mesa/OpenGL
development packages, and X11 development headers. On Debian or Ubuntu:

```bash
sudo apt-get install -y \
  cmake ninja-build xorg-dev libgl1-mesa-dev libglu1-mesa-dev \
  libsdl2-dev libcurl4-openssl-dev libssl-dev zlib1g-dev \
  imagemagick xvfb
make golden
```

To intentionally re-baseline screenshots, run `scripts/golden.sh update` and
review every changed PNG before committing it.

## Deploy over MTP

The deployment helper requires `gio` and an explicit target. By default it
replaces only the NRO:

```bash
make deploy MTP_DIR='mtp://DEVICE/1: SD Card/switch/pipensx'
```

Set `DEPLOY_CLEAN=1` to remove other remote files while preserving cached
artwork under `catalog/images/`:

```bash
make deploy MTP_DIR='mtp://DEVICE/1: SD Card/switch/pipensx' DEPLOY_CLEAN=1
```

## Cleanup and secret audit

```bash
make clean
make audit  # requires gitleaks in PATH
```
