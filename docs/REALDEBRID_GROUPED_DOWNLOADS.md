# Completed Real-Debrid selections

The transfer reuses a completed account torrent when its server selection
contains all requested files. Extra server-selected files do not cause a new
selection. Separate links are indexed by server selection order, independently
of installation order (base, update, DLC) or skipped files.

When links are grouped, `RemoteArchive` reads ZIP/ZIP64 central directories through
bounded HTTPS ranges and validates all needed member paths, lengths, compression
methods and local headers before installing the first package. It downloads
only the chosen members. ZIP Stored and raw Deflate members, data descriptors,
Unicode names and 64-bit offsets are supported. It uses bounded streaming
buffers rather than copying the full archive to RAM or SD. Multiple independent
ZIP links are supported. Existing complete local package caches are preferred.

The selected member flows into the existing NSP/NSZ installer or file downloader;
package ordering, journals, FAT32 split caching and port deployment are retained.
Installed package counts remain truthful on errors, including failure to copy a
selected port file. Grouped download failures never create per-file cloud torrents.
The original account torrent is retained.

A genuinely incomplete finalized server selection can still require one complete
replacement selection. Its actual cloud progress is polled and reported. A partial
link while cloud progress is below 100% does not make a torrent ready.

Encrypted or multi-volume ZIPs, other compression methods, missing/ambiguous
members, unknown HTTP length, ignored HTTP ranges or malformed metadata fail
explicitly. The metadata budget is 16 MiB / 65,536 directory entries; it does not
limit package payload size. CRC is checked for full stored reads and Deflate
reads, including resumed Deflate streams; stored byte-range resumes also retain
the package installer's normal content validation.

Tests: `test_remote_archive` exercises stored/Deflate/ZIP64, sparse offsets beyond
5 GiB, Unicode, bounded ranges, resumption, cancellation, CRC, truncation,
encryption and ignored ranges. `test_debrid_transfer` covers grouped NSP/NSZ,
nested base/update/DLC, port data, unused files, complete caches, partial installed
tasks, missing members before commits and completed account supersets with
individual links. Build shared C++17 tests and the Switch NRO.

Format reference: https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT

Physical investigation: Truck Sim Collection returns RAR4 (not ZIP), with only 676 bytes of header overhead for 928,895,364 selected payload bytes. The reader also indexes stored RAR4 by bounded header requests, including LARGE 64-bit sizes, compact UTF-16 names and backslash separators, skips unselected payloads and verifies header/member CRC. Encrypted/split/compressed RAR members are explicitly unsupported; the Real-Debrid stored container requires no proprietary decompressor. Reference header definitions were checked against the official UnRAR source from https://www.rarlab.com/rar_add.htm.
