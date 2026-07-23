// Round-trips the bug-report wire format entirely on the host: build framed
// chunks, reassemble them the way scripts/decode_report.py does, inflate, and
// assert the reconstructed tail matches. Guards the chunk-count budget and the
// trim-to-fit path. Header layout is documented in src/app/bug_report.hpp.

#include "app/bug_report.hpp"

#include <zlib.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pipensx;

namespace {

std::uint16_t readBe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint32_t readBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

// Mirror the decoder: validate headers, order by index, concat payloads,
// inflate, verify crc. Returns the reconstructed tail.
std::string reassemble(const BugReport& report) {
    const std::size_t total = report.chunks.size();
    std::vector<const std::vector<std::uint8_t>*> ordered(total, nullptr);
    std::uint16_t session = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressedLen = 0;

    for (const auto& chunk : report.chunks) {
        assert(chunk.size() >= kBugReportHeaderSize);
        assert(chunk[0] == 'P' && chunk[1] == 'Z');
        assert(chunk[2] == kBugReportFormatVersion);
        std::uint16_t s = readBe16(&chunk[3]);
        std::uint8_t idx = chunk[5];
        std::uint8_t tot = chunk[6];
        assert(tot == total);
        assert(idx < total);
        assert(ordered[idx] == nullptr); // no dupes
        if (idx == 0) {
            session = s;
            crc = readBe32(&chunk[8]);
            compressedLen = readBe32(&chunk[12]);
        } else {
            assert(s == session);
            assert(readBe32(&chunk[8]) == crc);
            assert(readBe32(&chunk[12]) == compressedLen);
        }
        ordered[idx] = &chunk;
    }
    assert(session == report.sessionId);

    std::vector<std::uint8_t> compressed;
    for (const auto* chunk : ordered) {
        assert(chunk != nullptr); // every index present
        compressed.insert(compressed.end(),
                          chunk->begin() + kBugReportHeaderSize, chunk->end());
    }
    assert(compressed.size() == compressedLen);

    std::string out(report.encodedTail.size(), '\0');
    uLongf outLen = static_cast<uLongf>(out.size());
    if (!out.empty()) {
        int rc = uncompress(reinterpret_cast<Bytef*>(&out[0]), &outLen,
                            compressed.data(),
                            static_cast<uLong>(compressed.size()));
        assert(rc == Z_OK);
        out.resize(outLen);
    }

    std::uint32_t got = static_cast<std::uint32_t>(
        crc32(crc32(0L, Z_NULL, 0),
              reinterpret_cast<const Bytef*>(out.data()),
              static_cast<uInt>(out.size())));
    assert(got == crc);
    return out;
}

} // namespace

// When given an output directory, dump a multi-chunk report there so the
// Python decoder (tests/test_bug_report_decode.py) can validate the wire format
// against this C++ builder cross-implementation.
void dumpFixtures(const std::string& dir) {
    std::string big;
    for (int i = 0; i < 4000; ++i)
        big += "[" + std::to_string(i) + "] peer 10.0.0." +
               std::to_string(i % 256) + " piece " + std::to_string(i) + "\n";
    BugReport report = buildBugReport(big, kBugReportDefault, 0xBEEF);

    for (std::size_t i = 0; i < report.chunks.size(); ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "%s/chunk_%02zu.bin", dir.c_str(), i);
        FILE* f = std::fopen(name, "wb");
        assert(f);
        std::fwrite(report.chunks[i].data(), 1, report.chunks[i].size(), f);
        std::fclose(f);
    }
    std::string exp = dir + "/expected.txt";
    FILE* f = std::fopen(exp.c_str(), "wb");
    assert(f);
    std::fwrite(report.encodedTail.data(), 1, report.encodedTail.size(), f);
    std::fclose(f);
}

int main(int argc, char** argv) {
    // Small tail fits in a single mode's grid with room to spare, round-trips
    // exactly, and is not marked truncated.
    {
        std::string log = "[startup] boot\n[meta] name='Test'\n"
                          "[diagnostic] level=error stage=net tag=timeout\n";
        BugReport report = buildBugReport(log, kBugReportDefault, 0x1234);
        assert(report.chunks.size() >= 1);
        assert(report.chunks.size() <= kBugReportDefault.maxChunks);
        assert(!report.truncated);
        assert(report.sessionId == 0x1234);
        assert(report.chunks[0][7] == 0x00); // default flag
        assert(reassemble(report) == log);
    }

    // Empty tail still yields one valid chunk.
    {
        BugReport report = buildBugReport("", kBugReportDefault, 0);
        assert(report.chunks.size() == 1);
        assert(reassemble(report).empty());
    }

    // Detailed flag rides in the header.
    {
        BugReport report = buildBugReport("hello\n", kBugReportDetailed, 7);
        assert(report.chunks[0][7] == 0x01);
        assert(reassemble(report) == "hello\n");
    }

    // A log far larger than any grid gets trimmed to fit, keeps the most
    // recent bytes, stays within the chunk budget, and still round-trips to
    // whatever survived the trim.
    {
        std::string big;
        for (int i = 0; i < 20000; ++i)
            big += "[" + std::to_string(i) + "] peer 10.0.0." +
                   std::to_string(i % 256) + " piece " + std::to_string(i) +
                   " request queued\n";
        BugReport report = buildBugReport(big, kBugReportDefault, 0xABCD);
        assert(report.truncated);
        assert(report.chunks.size() <= kBugReportDefault.maxChunks);
        // Kept the tail, dropped the head.
        assert(big.size() > report.encodedTail.size());
        assert(big.compare(big.size() - report.encodedTail.size(),
                           report.encodedTail.size(), report.encodedTail) == 0);
        assert(reassemble(report) == report.encodedTail);

        // Detailed mode fits more of the same log than default.
        BugReport detailed = buildBugReport(big, kBugReportDetailed, 0xABCD);
        assert(detailed.chunks.size() <= kBugReportDetailed.maxChunks);
        assert(detailed.encodedTail.size() > report.encodedTail.size());
        assert(reassemble(detailed) == detailed.encodedTail);
    }

    if (argc > 1)
        dumpFixtures(argv[1]);

    std::puts("bug report tests passed");
    return 0;
}
