#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pipensx::install {

struct PackageCallbacks {
    std::function<bool(const std::string&, uint64_t)> beginFile;
    std::function<bool(uint64_t)> setFileSize;
    std::function<bool(const uint8_t*, size_t)> writeFile;
    std::function<bool()> endFile;
    // Optional. Asked once per PFS0 entry (with the announced .nca name)
    // before any processing; returning true drops the entry: its bytes are
    // consumed raw with no NCZ decode, AES, hashing or installer callbacks
    // (PERF_PLAN 3.4 — delta fragments of updates).
    std::function<bool(const std::string&)> skipFile;
};

// Parser state at a resumable safe point (IMPROVEMENT_PLAN F-B). A safe
// point is an input position where every previously consumed byte has been
// fully digested and the remaining machine state needs no zstd decoder
// internals: between PFS0 entries, anywhere inside a raw (non-NCZ) or
// skipped entry, at NCZ block boundaries, or before a solid NCZ Zstandard
// stream has consumed its first byte. Undigested buffered input is rolled
// back instead of being serialized, so `consumed` may sit behind
// PackageStream::consumed(); the feeder resumes at `consumed`.
struct PackageStreamState {
    struct Entry {
        uint64_t offset = 0;
        uint64_t size = 0;
        std::string name;
    };
    struct Section {
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t cryptoType = 0;
        std::array<uint8_t, 16> key {};
        std::array<uint8_t, 16> counter {};
    };

    // Package-file input bytes fully digested at the safe point.
    uint64_t consumed = 0;
    // PFS0 parser.
    std::vector<Entry> entries;
    uint64_t headerSize = 0;
    uint64_t dataPosition = 0;
    uint64_t currentInputRemaining = 0;
    uint32_t entryIndex = 0;
    bool fileOpen = false;
    bool skipped = false;
    // NCZ decoder of the in-progress entry (when decoderPresent).
    bool decoderPresent = false;
    bool decoderHeaderReady = false;
    bool blockMode = false;
    uint64_t outputSize = 0;
    uint64_t outputPosition = 0;
    uint64_t blockSize = 0;
    uint32_t nextBlock = 0;
    std::vector<Section> sections;
    std::vector<uint32_t> blockSizes;
};

class PackageStream {
public:
    PackageStream(bool compressed, PackageCallbacks callbacks,
                  std::string telemetryTag = {});
    ~PackageStream();

    PackageStream(const PackageStream&) = delete;
    PackageStream& operator=(const PackageStream&) = delete;

    bool write(const uint8_t* data, size_t size);
    bool finish();
    uint64_t consumed() const;
    const std::string& error() const;

    // Fills `out` and returns true when the stream currently sits at (or can
    // roll undigested input back to) a resumable safe point. Never fails the
    // stream; a false return only means "no checkpoint here".
    bool checkpoint(PackageStreamState& out) const;
    // Restores a freshly constructed stream to a checkpointed safe point.
    // Digested content callbacks (beginFile/setFileSize/skipFile and prior
    // writes) are NOT replayed; the next write() must supply package bytes
    // starting at state.consumed. Fails (and poisons the stream) when the
    // state is inconsistent.
    bool restore(const PackageStreamState& state);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipensx::install
