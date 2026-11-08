# Client version

`scripts/version.py` resolves the application version for PC and Switch builds:

1. `PIPENSX_VERSION` explicitly supplied by a release job or developer.
2. The nearest reachable Git version tag. Local changes or commits after that
   tag add `-dev.<distance>+g<commit>[.dirty]`.
3. `VERSION` when building an exported source tree without Git metadata.

Update `VERSION` when preparing a release so source archives retain their
release number. A leading `v` in an explicit version is removed. Invalid or
overlong versions stop the build.

For example, `PIPENSX_VERSION=1.7.0 make switch` builds a release identifying
itself as `pipensx/1.7.0`. The same override works with `make -f Makefile.pc`.
PC builds keep a version stamp and rebuild when this override changes.

`src/core/client_identity.h` supplies the HTTP User-Agent and BitTorrent client
name. Tracker requests, web seeds, metadata, catalog, debrid, update and AutoRun
HTTP requests use the same application version. The standalone probe utility
keeps its separate diagnostic identity.

Torrent and magnet peer IDs share the `-PNxyz0-` prefix, where `x`, `y`, `z`
encode major, minor and patch using `0–9A–Z`. The remaining twelve bytes are
random. Components above 35 use the unknown prefix `-PN0000-`; the complete
version remains available in the
[BEP-10 `v` field](https://www.bittorrent.org/beps/bep_0010.html), including in
metadata/PEX negotiation and upload-only handshakes.

The complete version appears in the application and companion API. NACP
metadata and stable-update comparisons use the numeric major/minor/patch
projection, since development builds also carry commit and dirty suffixes.
