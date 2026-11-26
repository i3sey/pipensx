# RAR reader fixtures

Small reference archives from libarchive 3.7.2, decoded from the upstream
uuencoded files without changing archive contents:

- `rar4.rar`: [test_read_format_rar_subblock.rar.uu](https://github.com/libarchive/libarchive/blob/v3.7.2/libarchive/test/test_read_format_rar_subblock.rar.uu)
- `rar5.rar`: [test_read_format_rar5_stored.rar.uu](https://github.com/libarchive/libarchive/blob/v3.7.2/libarchive/test/test_read_format_rar5_stored.rar.uu)

These archives contain text files, not games. Tests combine them with a generated
GBA header fixture to verify preparation through both RAR readers.
See [upstream license](COPYING.libarchive).

Compressed reader fixtures from the same release:

- `rar4-compressed.rar`: first regular member of [normal compression](https://github.com/libarchive/libarchive/blob/v3.7.2/libarchive/test/test_read_format_rar_compress_normal.rar.uu). Original main header and the first file header/compressed data are retained; other members (including links) were removed and an end header was added. Verified with UnRAR 7.00.
- `rar5-compressed.rar`: [RAR5 compressed](https://github.com/libarchive/libarchive/blob/v3.7.2/libarchive/test/test_read_format_rar5_compressed.rar.uu)

`rar4-large-lzss.rar` is the unmodified [multi-LZSS-block reference](https://github.com/libarchive/libarchive/blob/v3.7.2/libarchive/test/test_read_format_rar_multi_lzss_blocks.rar.uu). UnRAR 7.00 validates it, but libarchive 3.7.2 produces different data and a CRC error. The preparation service must preserve inputs and refuse publication for this decoder failure.
