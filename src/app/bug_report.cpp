#include "app/bug_report.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace pipensx {

namespace {

// Deflate `data` at max level. Returns the zlib stream (with header/adler), the
// same framing Python's zlib.decompress expects.
std::vector<std::uint8_t> deflate(const std::string& data) {
    uLongf bound = compressBound(static_cast<uLong>(data.size()));
    std::vector<std::uint8_t> out(bound ? bound : 1);
    uLongf outLen = bound;
    int rc = compress2(out.data(), &outLen,
                       reinterpret_cast<const Bytef*>(data.data()),
                       static_cast<uLong>(data.size()), Z_BEST_COMPRESSION);
    if (rc != Z_OK)
        return {};
    out.resize(outLen);
    return out;
}

void putBe16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void putBe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::size_t chunkCount(std::size_t compressedLen, std::size_t perChunk) {
    if (compressedLen == 0)
        return 1;
    return (compressedLen + perChunk - 1) / perChunk;
}

} // namespace

BugReport buildBugReport(const std::string& logTail,
                         const BugReportConfig& config,
                         std::uint16_t sessionId) {
    const std::size_t perChunk = std::max<std::size_t>(1, config.chunkPayloadBytes);
    const std::size_t maxChunks = std::max<std::size_t>(1, config.maxChunks);
    const std::size_t maxCompressed = perChunk * maxChunks;

    std::string tail = logTail;
    std::vector<std::uint8_t> compressed = deflate(tail);

    // Trim the oldest bytes until the compressed stream fits the grid. Each
    // pass scales the kept suffix toward the capacity (0.9 margin so we
    // converge from above rather than oscillate), then snaps the cut to the
    // next line boundary so a partial first line never confuses the reader.
    bool truncated = false;
    for (int guard = 0; guard < 64 && compressed.size() > maxCompressed &&
                        !tail.empty();
         ++guard) {
        truncated = true;
        double ratio = static_cast<double>(maxCompressed) /
                       static_cast<double>(compressed.size());
        std::size_t keep = static_cast<std::size_t>(
            static_cast<double>(tail.size()) * ratio * 0.9);
        if (keep >= tail.size())
            keep = tail.size() - 1;
        std::size_t cut = tail.size() - keep;
        std::size_t nl = tail.find('\n', cut);
        if (nl != std::string::npos && nl + 1 < tail.size())
            cut = nl + 1;
        tail.erase(0, cut);
        compressed = deflate(tail);
    }

    const std::uint32_t crc = static_cast<std::uint32_t>(
        crc32(crc32(0L, Z_NULL, 0),
              reinterpret_cast<const Bytef*>(tail.data()),
              static_cast<uInt>(tail.size())));
    const std::uint32_t compressedLen =
        static_cast<std::uint32_t>(compressed.size());
    const std::size_t total = chunkCount(compressed.size(), perChunk);
    const std::uint8_t flags = config.detailed ? 0x01 : 0x00;

    BugReport report;
    report.sessionId = sessionId;
    report.encodedTail = tail;
    report.truncated = truncated;
    report.chunks.reserve(total);
    for (std::size_t idx = 0; idx < total; ++idx) {
        std::vector<std::uint8_t> chunk;
        chunk.reserve(kBugReportHeaderSize + perChunk);
        chunk.push_back('P');
        chunk.push_back('Z');
        chunk.push_back(kBugReportFormatVersion);
        putBe16(chunk, sessionId);
        chunk.push_back(static_cast<std::uint8_t>(idx));
        chunk.push_back(static_cast<std::uint8_t>(total));
        chunk.push_back(flags);
        putBe32(chunk, crc);
        putBe32(chunk, compressedLen);
        const std::size_t start = idx * perChunk;
        const std::size_t end = std::min(start + perChunk, compressed.size());
        chunk.insert(chunk.end(), compressed.begin() + start,
                     compressed.begin() + end);
        report.chunks.push_back(std::move(chunk));
    }
    return report;
}

} // namespace pipensx
