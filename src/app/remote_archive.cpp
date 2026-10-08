#include "remote_archive.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <zlib.h>

namespace pipensx {
namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; }
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }
bool fail(std::string& e, const char* message) { e = message; return false; }

// RAR4 stores an optional UTF-16 name after a NUL-terminated fallback name.
// Decode its compact two-bit commands with explicit input/output bounds.
bool rarName(const uint8_t* data, size_t size, bool unicode, std::string& name) {
    size_t base = 0;
    while (base < size && data[base]) ++base;
    if (!unicode || base == size) {
        name.assign(reinterpret_cast<const char*>(data), base);
        return !name.empty();
    }
    size_t p = base + 1;
    if (p >= size) return false;
    const uint16_t high = uint16_t(data[p++]) << 8;
    unsigned flags = 0, bits = 0;
    std::vector<uint16_t> units;
    while (p < size) {
        if (!bits) { flags = data[p++]; bits = 8; }
        const unsigned command = flags >> 6; flags = (flags << 2) & 255; bits -= 2;
        if (command <= 1) {
            if (p >= size) return false;
            units.push_back(data[p++] | (command == 1 ? high : 0));
        } else if (command == 2) {
            if (size - p < 2) return false;
            units.push_back(u16(data + p)); p += 2;
        } else {
            if (p >= size) return false;
            const unsigned length = data[p++];
            unsigned correction = 0;
            if (length & 128) { if (p >= size) return false; correction = data[p++]; }
            const size_t count = (length & 127) + 2;
            if (units.size() > base || count > base - units.size()) return false;
            for (size_t i = 0; i < count; ++i) {
                const unsigned value = data[units.size()];
                units.push_back((length & 128) ? ((value + correction) & 255) | high : value);
            }
        }
        if (units.size() > 4096) return false;
    }
    name.clear();
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t value = units[i];
        if (value >= 0xd800 && value <= 0xdbff) {
            if (++i == units.size() || units[i] < 0xdc00 || units[i] > 0xdfff) return false;
            value = 0x10000 + ((value - 0xd800) << 10) + units[i] - 0xdc00;
        } else if (value >= 0xdc00 && value <= 0xdfff) return false;
        if (!value) return false;
        if (value < 128) name.push_back(static_cast<char>(value));
        else {
            const unsigned count = value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
            name.push_back(static_cast<char>((count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0) | (value >> (6 * (count - 1)))));
            for (unsigned j = count - 1; j; --j) name.push_back(static_cast<char>(0x80 | ((value >> (6 * (j - 1))) & 63)));
        }
    }
    return !name.empty();
}
}

RemoteArchive::RemoteArchive(std::string url, uint64_t bytes, RangeFetcher fetcher)
    : url_(std::move(url)), bytes_(bytes), fetcher_(std::move(fetcher)) {}

bool RemoteArchive::openRar4(const std::function<bool()>& stopped, std::string& error) {
    uint64_t cursor = 7, metadata = 0;
    size_t blocks = 0;
    bool main = false;
    directoryOffset_ = bytes_;
    while (cursor < bytes_) {
        if (stopped()) return false;
        std::vector<uint8_t> header;
        if (!readBytes(cursor, 7, header, stopped, error)) return false;
        const size_t length = u16(header.data() + 5);
        if (length < 7 || ++blocks > 65536 || metadata + length > (16u << 20))
            return fail(error, "Grouped RAR4 has an invalid header or exceeds the metadata limit.");
        metadata += length;
        if (!readBytes(cursor, length, header, stopped, error)) return false;
        const uint8_t* h = header.data();
        if ((crc32(0, h + 2, static_cast<uInt>(length - 2)) & 0xffff) != u16(h))
            return fail(error, "Grouped RAR4 header failed CRC validation.");
        const unsigned type = h[2], flags = u16(h + 3);
        uint64_t packed = 0;
        if (flags & 0x8000) {
            if (length < 11) return fail(error, "Truncated grouped RAR4 long header.");
            packed = u32(h + 7);
        }
        if (type == 0x73) {
            if (main || length < 13 || (flags & 0x81))
                return fail(error, "Encrypted or multi-volume grouped RAR4 is not supported.");
            main = true;
        } else if (type == 0x74) {
            const size_t nameOffset = (flags & 0x100) ? 40 : 32;
            if (!main || length < nameOffset)
                return fail(error, "Truncated grouped RAR4 file header.");
            Entry entry;
            entry.headerOffset = cursor; entry.dataOffset = cursor + length;
            entry.bytes = u32(h + 11); entry.packedBytes = u32(h + 7);
            if (flags & 0x100) {
                entry.packedBytes |= uint64_t(u32(h + 32)) << 32;
                entry.bytes |= uint64_t(u32(h + 36)) << 32;
            }
            packed = entry.packedBytes;
            const size_t nameBytes = u16(h + 26);
            if (!nameBytes || nameBytes > 4096 || nameBytes > length - nameOffset ||
                !rarName(h + nameOffset, nameBytes, (flags & 0x200) != 0, entry.path))
                return fail(error, "Invalid grouped RAR4 member name.");
            std::replace(entry.path.begin(), entry.path.end(), '\\', '/');
            entry.crc = u32(h + 16);
            entry.method = h[25] == 0x30 ? 0 : 0xffff;
            // Convert RAR encryption/split flags to an unsupported-entry flag;
            // unwanted unsupported members can be skipped by their packed size.
            entry.flags = flags & 7 ? 1 : 0;
            entry.symlink = h[15] == 3 && (u32(h + 28) & 0170000) == 0120000;
            if ((flags & 0xe0) != 0xe0 && !(h[24] < 20 && (u32(h + 28) & 0x10)))
                entries_.push_back(std::move(entry));
        } else if (type == 0x7b) {
            if (!main || (flags & 1)) return fail(error, "Grouped RAR4 requires another volume.");
            return true;
        } else if (!(flags & 0x4000)) {
            return fail(error, "Grouped RAR4 contains an unsupported mandatory header.");
        }
        if (length > bytes_ - cursor || packed > bytes_ - cursor - length)
            return fail(error, "Grouped RAR4 member extends beyond the download.");
        cursor += length + packed;
    }
    return main;
}

bool RemoteArchive::readBytes(uint64_t offset, size_t count, std::vector<uint8_t>& out,
                          const std::function<bool()>& stopped, std::string& error) const {
    if (offset > bytes_ || count > bytes_ - offset)
        return fail(error, "Grouped archive has an invalid byte range.");
    out.clear();
    out.reserve(count);
    if (!count) return !stopped();
    bool overflow = false;
    const bool ok = fetcher_(url_, offset, offset + count,
        [&](const uint8_t* p, size_t n) {
            if (n > count - out.size()) { overflow = true; return false; }
            out.insert(out.end(), p, p + n);
            return !stopped();
        }, stopped, error);
    if (stopped()) return false;
    if (error == kDebridRangeNotSupported)
        return fail(error, "Grouped archive server ignored HTTP Range; no new torrent was created.");
    if (overflow || (ok && out.size() != count))
        return fail(error, "Grouped archive returned an incomplete or oversized byte range.");
    return ok;
}

bool RemoteArchive::open(const std::function<bool()>& stopped, std::string& error) {
    entries_.clear();
    rar4_ = false;
    std::vector<uint8_t> signature;
    if (!readBytes(0, 7, signature, stopped, error)) return false;
    if (std::memcmp(signature.data(), "Rar!\x1a\x07\x00", 7) == 0) {
        rar4_ = true;
        return openRar4(stopped, error);
    }
    if (bytes_ < 22) return fail(error, "Grouped download is not a valid ZIP archive.");
    const size_t tailBytes = static_cast<size_t>(std::min<uint64_t>(bytes_, 65557));
    const uint64_t tailOffset = bytes_ - tailBytes;
    std::vector<uint8_t> tail;
    if (!readBytes(tailOffset, tailBytes, tail, stopped, error)) return false;
    size_t eocd = tail.size();
    for (size_t p = tail.size() - 22;; --p) {
        if (u32(tail.data() + p) == 0x06054b50 &&
            p + 22 + u16(tail.data() + p + 20) == tail.size()) { eocd = p; break; }
        if (!p) break;
    }
    if (eocd == tail.size()) {
        std::vector<uint8_t> signature;
        if (!readBytes(0, 8, signature, stopped, error)) return false;
        char detail[160];
        std::snprintf(detail, sizeof(detail),
            "Grouped download is not supported ZIP/RAR4 (size=%llu signature=%02x%02x%02x%02x%02x%02x%02x%02x).",
            (unsigned long long)bytes_, signature[0], signature[1], signature[2], signature[3],
            signature[4], signature[5], signature[6], signature[7]);
        error = detail;
        return false;
    }
    const uint8_t* end = tail.data() + eocd;
    if (u16(end + 4) || u16(end + 6) || u16(end + 8) != u16(end + 10))
        return fail(error, "Multi-volume grouped archive is not supported.");
    uint64_t count = u16(end + 10), directoryBytes = u32(end + 12);
    directoryOffset_ = u32(end + 16);
    uint64_t directoryLimit = tailOffset + eocd;
    if (count == 0xffff || directoryBytes == 0xffffffff || directoryOffset_ == 0xffffffff) {
        if (directoryLimit < 20) return fail(error, "Missing ZIP64 locator.");
        std::vector<uint8_t> locator, record;
        if (!readBytes(directoryLimit - 20, 20, locator, stopped, error)) return false;
        if (u32(locator.data()) != 0x07064b50 || u32(locator.data() + 4) ||
            u32(locator.data() + 16) != 1)
            return fail(error, "Invalid ZIP64 locator or multiple volumes.");
        const uint64_t recordOffset = u64(locator.data() + 8);
        if (recordOffset > directoryLimit - 20 || directoryLimit - 20 - recordOffset < 56)
            return fail(error, "Invalid ZIP64 directory offset.");
        if (!readBytes(recordOffset, 56, record, stopped, error)) return false;
        const uint8_t* r = record.data();
        if (u32(r) != 0x06064b50 || u64(r + 4) < 44 ||
            u64(r + 4) > directoryLimit - 20 - recordOffset - 12 ||
            u32(r + 16) || u32(r + 20) || u64(r + 24) != u64(r + 32))
            return fail(error, "Invalid ZIP64 directory record.");
        count = u64(r + 32); directoryBytes = u64(r + 40); directoryOffset_ = u64(r + 48);
        directoryLimit = recordOffset;
    }
    // Bound metadata, not package size; payloads can exceed 4 GiB.
    if (count > 65536 || directoryBytes > (16u << 20) ||
        directoryOffset_ > directoryLimit || directoryBytes > directoryLimit - directoryOffset_ ||
        count > directoryBytes / 46)
        return fail(error, "Grouped archive directory is invalid or exceeds the metadata limit.");
    std::vector<uint8_t> directory;
    if (!readBytes(directoryOffset_, static_cast<size_t>(directoryBytes), directory, stopped, error)) return false;
    size_t cursor = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (stopped()) return false;
        if (directory.size() - cursor < 46 || u32(directory.data() + cursor) != 0x02014b50)
            return fail(error, "Invalid grouped archive central file header.");
        const uint8_t* h = directory.data() + cursor;
        const size_t nameBytes = u16(h + 28), extraBytes = u16(h + 30), commentBytes = u16(h + 32);
        const size_t length = 46 + nameBytes + extraBytes + commentBytes;
        if (!nameBytes || nameBytes > 4096 || length > directory.size() - cursor)
            return fail(error, "Invalid grouped archive file name or header length.");
        Entry entry;
        entry.path.assign(reinterpret_cast<const char*>(h + 46), nameBytes);
        entry.flags = u16(h + 8); entry.method = u16(h + 10); entry.crc = u32(h + 16);
        entry.symlink = ((u32(h + 38) >> 16) & 0170000) == 0120000;
        entry.packedBytes = u32(h + 20); entry.bytes = u32(h + 24); entry.headerOffset = u32(h + 42);
        uint32_t disk = u16(h + 34);
        const bool zip64 = entry.bytes == 0xffffffff || entry.packedBytes == 0xffffffff ||
                           entry.headerOffset == 0xffffffff || disk == 0xffff;
        bool found64 = false;
        for (size_t p = 46 + nameBytes; p < 46 + nameBytes + extraBytes;) {
            const size_t remaining = 46 + nameBytes + extraBytes - p;
            if (remaining < 4 || u16(h + p + 2) > remaining - 4)
                return fail(error, "Truncated grouped archive extra field.");
            const size_t fieldBytes = u16(h + p + 2);
            if (u16(h + p) == 1 && zip64) {
                size_t q = p + 4, limit = q + fieldBytes;
                auto extended = [&](uint64_t& value) {
                    if (value != 0xffffffff) return true;
                    if (limit - q < 8) return false;
                    value = u64(h + q); q += 8; return true;
                };
                if (!extended(entry.bytes) || !extended(entry.packedBytes) || !extended(entry.headerOffset))
                    return fail(error, "Missing grouped archive64 file sizes or offsets.");
                if (disk == 0xffff) {
                    if (limit - q < 4) return fail(error, "Missing ZIP64 disk field.");
                    disk = u32(h + q);
                }
                found64 = true;
            }
            p += 4 + fieldBytes;
        }
        if ((zip64 && !found64) || disk || entry.headerOffset >= directoryOffset_)
            return fail(error, "Invalid grouped archive64 member or multiple volumes.");
        // Directories/unwanted unsupported entries are never extracted.
        if (entry.path.back() != '/') entries_.push_back(std::move(entry));
        cursor += length;
    }
    return true;
}

bool RemoteArchive::prepare(size_t index, const std::function<bool()>& stopped, std::string& error) {
    if (index >= entries_.size()) return fail(error, "Missing grouped archive member.");
    Entry& e = entries_[index];
    if (e.symlink || e.path.find('\0') != std::string::npos ||
        (e.flags & (1 | 0x40 | 0x2000)) || (e.method != 0 && e.method != 8))
        return fail(error, "Selected grouped archive member is encrypted or uses unsupported compression.");
    if (e.method == 0 && e.bytes != e.packedBytes)
        return fail(error, "Stored grouped archive member has inconsistent sizes.");
    if (rar4_) { e.prepared = true; return !stopped(); }
    std::vector<uint8_t> header;
    // The directory already supplies the expected name length: validate the
    // local header and name in one request, reducing per-DLC round trips.
    if (!readBytes(e.headerOffset, 30 + e.path.size(), header, stopped, error)) return false;
    const uint8_t* h = header.data();
    if (u32(h) != 0x04034b50 || u16(h + 6) != e.flags || u16(h + 8) != e.method || u16(h + 26) != e.path.size())
        return fail(error, "Grouped archive local and central headers disagree.");
    const uint64_t length = 30u + u16(h + 26) + u16(h + 28);
    if (e.headerOffset > directoryOffset_ || length > directoryOffset_ - e.headerOffset)
        return fail(error, "Grouped archive local header exceeds its data region.");
    e.dataOffset = e.headerOffset + length;
    if (e.packedBytes > directoryOffset_ - e.dataOffset)
        return fail(error, "Grouped archive member extends into the directory.");
    if (std::memcmp(header.data() + 30, e.path.data(), e.path.size()) != 0)
        return fail(error, "Grouped archive member name does not match its directory.");
    e.prepared = true;
    return true;
}

bool RemoteArchive::read(size_t index, uint64_t offset, uint64_t end,
    const std::function<bool(const uint8_t*, size_t)>& sink,
    const std::function<bool()>& stopped, std::string& error) const {
    if (index >= entries_.size() || !entries_[index].prepared)
        return fail(error, "Grouped archive member has not been validated.");
    const Entry& e = entries_[index];
    const uint64_t limit = end ? end : e.bytes;
    if (offset > limit || limit > e.bytes) return fail(error, "Invalid grouped archive member range.");
    if (offset == limit) return !stopped();
    uint64_t produced = e.method == 0 ? offset : 0, received = 0;
    uLong crc = crc32(0, Z_NULL, 0);
    bool bad = false, finished = false;
    auto emit = [&](const uint8_t* data, size_t n) {
        if (n > e.bytes - produced) { bad = true; return false; }
        // zlib CRC length is uInt; input/output buffers are bounded below.
        crc = crc32(crc, data, static_cast<uInt>(n));
        const uint64_t begin = std::max(produced, offset), stop = std::min(produced + n, limit);
        produced += n;
        return begin >= stop || sink(data + (begin - (produced - n)), static_cast<size_t>(stop - begin));
    };
    z_stream stream{};
    std::vector<uint8_t> output(256u << 10);
    if (e.method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return fail(error, "Unable to initialize grouped archive decompression.");
    const uint64_t start = e.method == 0 ? offset : 0;
    const uint64_t packedEnd = e.method == 0 ? limit : e.packedBytes;
    const bool ok = fetcher_(url_, e.dataOffset + start, e.dataOffset + packedEnd,
        [&](const uint8_t* p, size_t n) {
            if (stopped() || n > packedEnd - start - received) { bad = !stopped(); return false; }
            received += n;
            // Also bound arbitrary injected fetcher chunks before zlib's uInt.
            while (n) {
                const size_t step = std::min<size_t>(n, 256u << 10);
                if (e.method == 0) { if (!emit(p, step)) return false; }
                else {
                    if (finished) { bad = true; return false; }
                    stream.next_in = const_cast<Bytef*>(p); stream.avail_in = static_cast<uInt>(step);
                    do {
                        stream.next_out = output.data(); stream.avail_out = output.size();
                        const int rc = inflate(&stream, Z_NO_FLUSH);
                        if (rc == Z_BUF_ERROR && !stream.avail_in) break;
                        if (rc != Z_OK && rc != Z_STREAM_END) { bad = true; return false; }
                        if (!emit(output.data(), output.size() - stream.avail_out)) return false;
                        if (rc == Z_STREAM_END) {
                            finished = true;
                            if (stream.avail_in || n != step) { bad = true; return false; }
                            break;
                        }
                    } while (stream.avail_in || !stream.avail_out);
                }
                p += step; n -= step;
            }
            return !stopped();
        }, stopped, error);
    if (e.method == 8) inflateEnd(&stream);
    if (stopped()) return false;
    if (error == kDebridRangeNotSupported)
        return fail(error, "Grouped archive server ignored HTTP Range; no new torrent was created.");
    if (bad || (ok && (received != packedEnd - start || produced != (e.method == 0 ? limit : e.bytes) ||
        (e.method == 8 && !finished) || ((e.method == 8 || (offset == 0 && limit == e.bytes)) && crc != e.crc))))
        return fail(error, "Grouped archive member failed size/decompression/CRC validation.");
    return ok;
}
} // namespace pipensx
