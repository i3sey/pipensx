#pragma once
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <zlib.h>

namespace zip_fixture {
inline void number(std::string& s, uint64_t value, unsigned bytes) {
    while (bytes--) { s.push_back(static_cast<char>(value)); value >>= 8; }
}
struct File { std::string path, data; bool deflated = false; };
inline std::string rarHeader(unsigned type, unsigned flags, const std::string& body) {
    std::string header;
    number(header, 0, 2); number(header, type, 1); number(header, flags, 2);
    number(header, 7 + body.size(), 2); header += body;
    const auto crc = crc32(0, reinterpret_cast<const Bytef*>(header.data() + 2), header.size() - 2);
    header[0] = static_cast<char>(crc); header[1] = static_cast<char>(crc >> 8);
    return header;
}
inline std::string rar4(const std::vector<File>& files, bool large = false) {
    std::string out("Rar!\x1a\x07\x00", 7);
    out += rarHeader(0x73, 0, std::string(6, '\0'));
    for (const auto& f : files) {
        std::string body;
        number(body, f.data.size(), 4); number(body, f.data.size(), 4); number(body, 2, 1);
        number(body, crc32(0, reinterpret_cast<const Bytef*>(f.data.data()), f.data.size()), 4);
        number(body, 0, 4); number(body, 29, 1); number(body, 0x30, 1);
        number(body, f.path.size(), 2); number(body, 0x20, 4);
        if (large) { number(body, 0, 4); number(body, 0, 4); }
        body += f.path;
        out += rarHeader(0x74, 0x8000 | (large ? 0x100 : 0), body); out += f.data;
    }
    out += rarHeader(0x7b, 0, "");
    return out;
}
// The prefix offset allows a sparse ZIP64 fixture beyond 4 GiB without
// allocating a multi-GiB test buffer. Returned data starts at prefixOffset.
inline std::string build(const std::vector<File>& files, bool zip64 = false,
                         uint64_t prefixOffset = 0) {
    std::string out, directory;
    for (const auto& file : files) {
        std::string packed = file.data;
        if (file.deflated) {
            z_stream stream{};
            assert(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
            packed.resize(deflateBound(&stream, file.data.size()));
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(file.data.data()));
            stream.avail_in = file.data.size();
            stream.next_out = reinterpret_cast<Bytef*>(packed.data());
            stream.avail_out = packed.size();
            assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
            packed.resize(stream.total_out);
            deflateEnd(&stream);
        }
        const uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(file.data.data()), file.data.size());
        const uint64_t offset = prefixOffset + out.size();
        number(out, 0x04034b50, 4); number(out, zip64 ? 45 : 20, 2);
        number(out, 8, 2); // Data descriptor, as produced by streaming cloud ZIPs.
        number(out, file.deflated ? 8 : 0, 2); number(out, 0, 4);
        number(out, 0, 4); number(out, zip64 ? 0xffffffff : 0, 4); number(out, zip64 ? 0xffffffff : 0, 4);
        number(out, file.path.size(), 2); number(out, zip64 ? 20 : 0, 2); out += file.path;
        if (zip64) { number(out, 1, 2); number(out, 16, 2); number(out, file.data.size(), 8); number(out, packed.size(), 8); }
        out += packed;
        number(out, 0x08074b50, 4); number(out, crc, 4);
        number(out, packed.size(), zip64 ? 8 : 4); number(out, file.data.size(), zip64 ? 8 : 4);
        number(directory, 0x02014b50, 4); number(directory, 45, 2); number(directory, zip64 ? 45 : 20, 2);
        number(directory, 8, 2); number(directory, file.deflated ? 8 : 0, 2); number(directory, 0, 4);
        number(directory, crc, 4); number(directory, zip64 ? 0xffffffff : packed.size(), 4);
        number(directory, zip64 ? 0xffffffff : file.data.size(), 4);
        number(directory, file.path.size(), 2); number(directory, zip64 ? 28 : 0, 2);
        number(directory, 0, 2); number(directory, 0, 2); number(directory, 0, 2); number(directory, 0, 4);
        number(directory, zip64 ? 0xffffffff : offset, 4); directory += file.path;
        if (zip64) {
            number(directory, 1, 2); number(directory, 24, 2);
            number(directory, file.data.size(), 8); number(directory, packed.size(), 8); number(directory, offset, 8);
        }
    }
    const uint64_t directoryOffset = prefixOffset + out.size(); out += directory;
    if (zip64) {
        const uint64_t recordOffset = prefixOffset + out.size();
        number(out, 0x06064b50, 4); number(out, 44, 8); number(out, 45, 2); number(out, 45, 2);
        number(out, 0, 4); number(out, 0, 4); number(out, files.size(), 8); number(out, files.size(), 8);
        number(out, directory.size(), 8); number(out, directoryOffset, 8);
        number(out, 0x07064b50, 4); number(out, 0, 4); number(out, recordOffset, 8); number(out, 1, 4);
    }
    number(out, 0x06054b50, 4); number(out, 0, 2); number(out, 0, 2);
    number(out, zip64 ? 0xffff : files.size(), 2); number(out, zip64 ? 0xffff : files.size(), 2);
    number(out, zip64 ? 0xffffffff : directory.size(), 4); number(out, zip64 ? 0xffffffff : directoryOffset, 4);
    number(out, 0, 2);
    return out;
}
} // namespace zip_fixture
