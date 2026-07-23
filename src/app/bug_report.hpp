#pragma once

// Bug-report payload builder — turns a recent log tail into a set of binary
// chunks, each of which the UI renders as one QR code on a single screen. A
// reporter photographs that screen; scripts/decode_report.py decodes the QRs,
// reassembles the chunks and reconstructs the log. No log file ever leaves the
// console.
//
// This translation unit is deliberately platform-neutral (no borealis, no
// libnx, no qrcodegen): it compiles into the Switch app, the golden runner and
// the Makefile.pc test suite alike, so the wire format is unit-tested on PC.
//
// Wire format (per chunk, must stay in lockstep with scripts/decode_report.py):
//   offset size field
//   0      2    magic 'P','Z'
//   2      1    format version (kFormatVersion)
//   3      2    session id            (uint16, big-endian)
//   5      1    chunk index           (0-based)
//   6      1    chunk total
//   7      1    flags (bit0 = detailed mode)
//   8      4    crc32 of the UNCOMPRESSED log tail (uint32, big-endian)
//   12     4    total compressed length across all chunks (uint32, big-endian)
//   16     ..   this chunk's slice of the zlib stream
// The global fields (session id, total, crc32, compressed length) repeat in
// every chunk, so any single decoded chunk reveals the whole report's shape.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

// Error-correction level the UI should feed to qrcodegen for a mode's codes.
// Kept as a tiny enum here so this header stays free of the qrcodegen include;
// the view maps it to qrcodegen::QrCode::Ecc.
enum class QrEcc { Medium, Quartile };

struct BugReportConfig {
    std::size_t maxChunks;         // most QR codes that fit one screen
    std::size_t chunkPayloadBytes; // compressed bytes per code (governs density)
    QrEcc ecc;
    bool detailed;
};

// Photo-of-TV default: few, low-density, high-error-correction codes.
inline constexpr BugReportConfig kBugReportDefault{
    /*maxChunks=*/9, /*chunkPayloadBytes=*/180, QrEcc::Quartile,
    /*detailed=*/false};

// Screenshot / clean-photo "super detailed": many denser codes, more log.
inline constexpr BugReportConfig kBugReportDetailed{
    /*maxChunks=*/16, /*chunkPayloadBytes=*/640, QrEcc::Medium,
    /*detailed=*/true};

inline constexpr std::uint8_t kBugReportHeaderSize = 16;
inline constexpr std::uint8_t kBugReportFormatVersion = 1;

struct BugReport {
    std::vector<std::vector<std::uint8_t>> chunks; // one per QR code
    std::string encodedTail; // the (possibly trimmed) tail actually encoded
    std::uint16_t sessionId;
    bool truncated; // true if the tail was trimmed to fit the grid
};

// Compress logTail with zlib, split it into <= config.maxChunks framed chunks,
// trimming the oldest bytes of the tail (at a line boundary) until it fits.
// Always returns at least one chunk, even for an empty tail. sessionId groups
// the chunks and is echoed on screen so the dev can confirm a complete capture.
BugReport buildBugReport(const std::string& logTail,
                         const BugReportConfig& config,
                         std::uint16_t sessionId);

} // namespace pipensx
