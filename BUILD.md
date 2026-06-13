# pipensx — Build Instructions

## PC build (for development and testing)

Requirements: `gcc`, `make`, `libcurl` dev headers

```bash
# Install deps (Arch/Manjaro)
sudo pacman -S curl

# Build
make -f Makefile.pc

# Run (put any .torrent file)
./pipensx /path/to/file.torrent [output_dir]
```

## Nintendo Switch build (.nro)

### 1. Install devkitPro portlibs (one time)

```bash
sudo pacman -S switch-curl switch-mbedtls switch-zlib switch-miniupnpc
```

### 2. Build

```bash
make -f Makefile.switch
# Output: pipensx.nro
```

### 3. Deploy to Switch

Copy to SD card:
```
SD:/switch/pipensx.nro
```

Create folders (will be created automatically on first run):
```
SD:/switch/pipensx/torrents/   <- put .torrent files here
SD:/switch/pipensx/downloads/  <- files downloaded here
SD:/switch/pipensx/dht_nodes.bin  <- created automatically (DHT cache)
```

### 4. Usage on Switch

1. Launch via **hbmenu** (hold R while launching any game, or Album if using CFW)
2. The app will find the first `.torrent` in `/switch/pipensx/torrents/`
3. Shows progress on screen: `%`, speed, peer count, DHT nodes
4. Press **+** to exit

## Notes

- **DHT + PEX** are always active — peers found even if tracker is blocked
- DHT bootstrap nodes: `router.bittorrent.com`, `dht.transmissionbt.com`, `router.utorrent.com`
- DHT node cache is saved on exit and loaded on next start for faster bootstrapping
- Only `.torrent` files supported (no magnet links in v1)
- One torrent per session
- exFAT SD recommended (FAT32 has 4GB file size limit)
