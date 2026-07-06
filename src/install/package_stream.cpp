#include "package_stream.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <zstd.h>

extern "C" {
#include "../core/util.h"
}

#ifdef __SWITCH__
extern "C" {
#include <switch/crypto/aes_ctr.h>
}
#else
#include <openssl/aes.h>
#endif

namespace pipensx::install {
namespace {

uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read64(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}

uint64_t read64be(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<uint64_t>(p[i]);
    return value;
}

std::string hexPreview(const uint8_t* data, size_t size) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 3);
    for (size_t i = 0; i < size; ++i) {
        if (i)
            out.push_back(' ');
        out.push_back(hex[data[i] >> 4]);
        out.push_back(hex[data[i] & 0xf]);
    }
    return out;
}

struct PfsEntry {
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string name;
};

// Append-at-back / consume-from-front byte buffer with an O(1) consume cursor.
// consume() advances head_ instead of erase(begin, ...), which would memmove
// the whole remaining tail on every call — expensive at PFS0/NCZ boundaries
// where a small consume leaves a large tail. The consumed prefix is reclaimed
// lazily on the next append: O(1) clear when fully drained, otherwise a single
// move of just the (small) unconsumed remainder. Exposes the read-only subset
// of the std::vector surface the parsers use (data/size/empty/operator[]).
class ByteQueue {
public:
    const uint8_t* data() const { return buf_.data() + head_; }
    size_t size() const { return buf_.size() - head_; }
    bool empty() const { return head_ == buf_.size(); }
    uint8_t operator[](size_t index) const { return buf_[head_ + index]; }

    void append(const uint8_t* data, size_t size) {
        if (head_ > 0) {
            if (head_ == buf_.size())
                buf_.clear();
            else
                buf_.erase(buf_.begin(), buf_.begin() + head_);
            head_ = 0;
        }
        buf_.insert(buf_.end(), data, data + size);
    }

    // Caller guarantees count <= size().
    void consume(size_t count) { head_ += count; }

    void clear() {
        buf_.clear();
        head_ = 0;
    }

private:
    std::vector<uint8_t> buf_;
    size_t head_ = 0;
};

struct NczSection {
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t cryptoType = 0;
    std::array<uint8_t, 16> key {};
    std::array<uint8_t, 16> counter {};
};

class AesCtr {
public:
    static bool transform(const NczSection& section, uint64_t absoluteOffset,
                          uint8_t* data, size_t size) {
        if (section.cryptoType != 3 && section.cryptoType != 4)
            return true;

#ifdef __SWITCH__
        uint8_t counter[16] {};
        std::memcpy(counter, section.counter.data(), 8);
        uint64_t block = absoluteOffset >> 4;
        for (unsigned i = 0; i < 8; ++i)
            counter[15 - i] = static_cast<uint8_t>(block >> (i * 8));

        Aes128CtrContext aes;
        aes128CtrContextCreate(&aes, section.key.data(), counter);

        // Keystream starts at the block boundary; burn the intra-block
        // prefix so it lines up with absoluteOffset.
        size_t skip = static_cast<size_t>(absoluteOffset & 15);
        if (skip) {
            uint8_t scratch[16] {};
            aes128CtrCrypt(&aes, scratch, scratch, skip);
        }
        aes128CtrCrypt(&aes, data, data, size);
        return true;
#else
        AES_KEY aes;
        if (AES_set_encrypt_key(section.key.data(), 128, &aes) != 0)
            return false;
        size_t done = 0;
        while (done < size) {
            uint64_t position = absoluteOffset + done;
            uint64_t block = position >> 4;
            size_t skip = static_cast<size_t>(position & 15);
            uint8_t counter[16] {};
            std::memcpy(counter, section.counter.data(), 8);
            for (unsigned i = 0; i < 8; ++i)
                counter[15 - i] = static_cast<uint8_t>(block >> (i * 8));
            uint8_t stream[16];
            AES_encrypt(counter, stream, &aes);
            size_t count = std::min<size_t>(16 - skip, size - done);
            for (size_t i = 0; i < count; ++i)
                data[done + i] ^= stream[skip + i];
            done += count;
        }
        return true;
#endif
    }
};

class NczDecoder {
public:
    using Ready = std::function<bool(uint64_t)>;
    using Writer = std::function<bool(const uint8_t*, size_t)>;

    NczDecoder(uint64_t inputSize, std::string telemetryTag,
               Ready ready, Writer writer)
        : inputSize_(inputSize), inputRemaining_(inputSize), ready_(std::move(ready)),
          writer_(std::move(writer)), telemetryTag_(std::move(telemetryTag)) {
        telemetryStartMs_ = now_ms();
        resetTelemetry(telemetryStartMs_);
    }

    ~NczDecoder() {
        emitTelemetry(now_ms(), true, true);
        if (stream_)
            ZSTD_freeDStream(stream_);
    }

    bool write(const uint8_t* data, size_t size, std::string& error) {
        if (size > inputRemaining_) {
            error = "NCZ input exceeds its PFS0 entry.";
            return false;
        }
        inputRemaining_ -= size;
        telemetryInputBytes_ += size;
        telemetryTotalInputBytes_ += size;
        pending_.append(data, size);
        bool ok = true;
        if (!headerReady_ && !parseHeader(error))
            ok = error.empty();
        else if (headerReady_)
            ok = blockMode_ ? processBlocks(error) : processSolid(error);
        emitTelemetry(now_ms(), false, false);
        return ok;
    }

    // A decoder safe point needs no zstd internals: before the NCZ header is
    // digested (everything sits in pending_, which the checkpoint rolls
    // back), at block-mode block boundaries (blocks are compressed
    // independently), or before a solid Zstandard stream consumed its first
    // byte. Mid-frame solid state lives inside ZSTD_DStream and cannot be
    // serialized, so a solid entry in flight is not checkpointable.
    bool checkpointable() const {
        if (!headerReady_)
            return true;
        if (blockMode_)
            return true;
        return solidConsumed_ == 0;
    }

    size_t pendingSize() const { return pending_.size(); }

    void exportState(PackageStreamState& out) const {
        out.decoderPresent = true;
        out.decoderHeaderReady = headerReady_;
        if (!headerReady_)
            return;
        out.blockMode = blockMode_;
        out.outputSize = outputSize_;
        out.outputPosition = outputPosition_;
        out.blockSize = blockSize_;
        out.nextBlock = static_cast<uint32_t>(nextBlock_);
        out.sections.reserve(sections_.size());
        for (const auto& section : sections_) {
            PackageStreamState::Section stored;
            stored.offset = section.offset;
            stored.size = section.size;
            stored.cryptoType = section.cryptoType;
            stored.key = section.key;
            stored.counter = section.counter;
            out.sections.push_back(stored);
        }
        out.blockSizes = blockSizes_;
    }

    // Rebuilds a freshly constructed decoder at a checkpointed safe point.
    // ready_ and the NCA-header write are NOT replayed — the installer
    // restored its own side from the journal.
    bool restore(const PackageStreamState& in, uint64_t inputRemaining,
                 std::string& error) {
        if (inputRemaining > inputSize_) {
            error = "NCZ restore input position is invalid.";
            return false;
        }
        inputRemaining_ = inputRemaining;
        if (!in.decoderHeaderReady) {
            // Header never digested: everything was rolled back, the decoder
            // restarts fresh and re-parses the NCZ header from re-fed input.
            if (inputRemaining_ != inputSize_) {
                error = "NCZ restore expects a fresh decoder.";
                return false;
            }
            return true;
        }
        constexpr uint64_t ncaHeaderSize = 0x4000;
        if (in.sections.empty() ||
            in.outputPosition < ncaHeaderSize ||
            in.outputPosition > in.outputSize) {
            error = "NCZ restore section state is invalid.";
            return false;
        }
        uint64_t expect = ncaHeaderSize;
        sections_.clear();
        sections_.reserve(in.sections.size());
        for (const auto& stored : in.sections) {
            if (stored.offset != expect || stored.size == 0 ||
                stored.size > std::numeric_limits<uint64_t>::max() -
                                  stored.offset) {
                error = "NCZ restore sections are not contiguous.";
                return false;
            }
            NczSection section;
            section.offset = stored.offset;
            section.size = stored.size;
            section.cryptoType = stored.cryptoType;
            section.key = stored.key;
            section.counter = stored.counter;
            sections_.push_back(section);
            expect = stored.offset + stored.size;
        }
        if (expect != in.outputSize) {
            error = "NCZ restore output size mismatch.";
            return false;
        }
        outputSize_ = in.outputSize;
        outputPosition_ = in.outputPosition;
        currentSection_ = 0; // emit() advances lazily from the front
        blockMode_ = in.blockMode;
        headerReady_ = true;
        if (blockMode_) {
            bool sizeValid = false;
            for (unsigned exponent = 14; exponent <= 30; ++exponent)
                if (in.blockSize == (uint64_t{1} << exponent))
                    sizeValid = true;
            if (!sizeValid || in.blockSizes.empty() ||
                in.blockSizes.size() > (1u << 24) ||
                in.nextBlock > in.blockSizes.size()) {
                error = "NCZ restore block state is invalid.";
                return false;
            }
            uint64_t expectedPosition = in.nextBlock == in.blockSizes.size()
                ? outputSize_
                : ncaHeaderSize +
                      static_cast<uint64_t>(in.nextBlock) * in.blockSize;
            if (outputPosition_ != expectedPosition) {
                error = "NCZ restore is not at a block boundary.";
                return false;
            }
            blockSize_ = in.blockSize;
            blockSizes_ = in.blockSizes;
            nextBlock_ = in.nextBlock;
        } else {
            // Solid streams are only checkpointed untouched: nothing beyond
            // the raw NCA header has been emitted yet.
            if (in.nextBlock != 0 || !in.blockSizes.empty() ||
                outputPosition_ != ncaHeaderSize) {
                error = "NCZ restore solid state is invalid.";
                return false;
            }
            stream_ = ZSTD_createDStream();
            if (!stream_ || ZSTD_isError(ZSTD_initDStream(stream_))) {
                error = "Unable to initialize Zstandard decoder.";
                return false;
            }
        }
        return true;
    }

    bool finish(std::string& error) {
        if (inputRemaining_ != 0) {
            error = "NCZ stream ended before its declared size.";
            return false;
        }
        if (!headerReady_ && !parseHeader(error))
            return false;
        if (!headerReady_) {
            error = "Incomplete NCZ header.";
            return false;
        }
        if (blockMode_) {
            if (!processBlocks(error))
                return false;
            if (nextBlock_ != blockSizes_.size() || !pending_.empty()) {
                error = "Incomplete NCZ block stream.";
                return false;
            }
        } else {
            if (!processSolid(error))
                return false;
            if (!solidEnded_) {
                error = "Incomplete NCZ Zstandard stream.";
                return false;
            }
        }
        if (outputPosition_ != outputSize_) {
            error = "NCZ decompressed size mismatch.";
            return false;
        }
        return true;
    }

    uint64_t outputSize() const { return outputSize_; }

private:
    struct NczLayout {
        const char* name;
        bool hasCount;
        size_t countOffset;
        size_t firstEntryOffset;
        size_t entrySize;
        bool prefixMagic;
        bool entryMagic;
        size_t offsetOffset;
        size_t sizeOffset;
        size_t cryptoOffset;
        size_t keyOffset;
        size_t counterOffset;
    };

    enum class HeaderResult {
        Parsed,
        NeedMore,
        Invalid,
    };

    HeaderResult tryParseLayout(const NczLayout& layout, size_t& headerSize,
                                uint64_t& outputSize,
                                std::vector<NczSection>& parsed) const {
        constexpr size_t ncaHeaderSize = 0x4000;
        constexpr uint64_t hardMaxSections = 1u << 20;
        constexpr const char* sectionMagic = "NCZSECTN";
        uint64_t maxSections = hardMaxSections;
        uint64_t headerBase = ncaHeaderSize + layout.firstEntryOffset;
        if (inputSize_ <= headerBase)
            return HeaderResult::Invalid;
        maxSections = std::min<uint64_t>(
            maxSections, (inputSize_ - headerBase) / layout.entrySize);
        if (maxSections == 0)
            return HeaderResult::Invalid;

        if (layout.prefixMagic) {
            if (pending_.size() < ncaHeaderSize + 8)
                return HeaderResult::NeedMore;
            if (std::memcmp(pending_.data() + ncaHeaderSize,
                            sectionMagic, 8) != 0) {
                return HeaderResult::Invalid;
            }
        }

        uint64_t count = 0;
        if (layout.hasCount) {
            if (pending_.size() < ncaHeaderSize + layout.countOffset + 8)
                return HeaderResult::NeedMore;
            const uint8_t* countPtr =
                pending_.data() + ncaHeaderSize + layout.countOffset;
            count = read64(countPtr);
            if (count == 0 || count > maxSections)
                count = read64be(countPtr);
            if (count == 0 || count > maxSections)
                return HeaderResult::Invalid;
        } else {
            size_t position = ncaHeaderSize + layout.firstEntryOffset;
            while (count < maxSections) {
                if (pending_.size() < position + 8)
                    return HeaderResult::NeedMore;
                if (std::memcmp(pending_.data() + position,
                                sectionMagic, 8) != 0) {
                    break;
                }
                ++count;
                position += layout.entrySize;
            }
            if (count == 0 || count > maxSections)
                return HeaderResult::Invalid;
        }

        uint64_t headerSize64 = ncaHeaderSize + layout.firstEntryOffset +
                                count * layout.entrySize;
        if (headerSize64 > std::numeric_limits<size_t>::max())
            return HeaderResult::Invalid;
        headerSize = static_cast<size_t>(headerSize64);
        if (pending_.size() < headerSize)
            return HeaderResult::NeedMore;

        std::vector<NczSection> sections;
        sections.reserve(static_cast<size_t>(count) * 2);
        uint64_t previousEnd = ncaHeaderSize;
        outputSize = ncaHeaderSize;
        const uint8_t* entry =
            pending_.data() + ncaHeaderSize + layout.firstEntryOffset;
        for (uint64_t i = 0; i < count; ++i, entry += layout.entrySize) {
            if (layout.entryMagic &&
                std::memcmp(entry, sectionMagic, 8) != 0) {
                return HeaderResult::Invalid;
            }
            NczSection section;
            section.offset = read64(entry + layout.offsetOffset);
            section.size = read64(entry + layout.sizeOffset);
            section.cryptoType = read64(entry + layout.cryptoOffset);
            std::memcpy(section.key.data(), entry + layout.keyOffset, 16);
            std::memcpy(section.counter.data(),
                        entry + layout.counterOffset, 16);
            if (section.size == 0 ||
                section.size > std::numeric_limits<uint64_t>::max() -
                                   section.offset) {
                return HeaderResult::Invalid;
            }
            uint64_t sectionEnd = section.offset + section.size;
            if (sectionEnd <= previousEnd)
                continue;
            if (section.offset > previousEnd) {
                NczSection gap;
                gap.offset = previousEnd;
                gap.size = section.offset - previousEnd;
                sections.push_back(gap);
            }
            sections.push_back(section);
            previousEnd = sectionEnd;
            outputSize = std::max(outputSize, previousEnd);
        }

        parsed = std::move(sections);
        return HeaderResult::Parsed;
    }

    bool parseHeader(std::string& error) {
        constexpr size_t ncaHeaderSize = 0x4000;
        if (pending_.size() < ncaHeaderSize + 8)
            return true;

        static const NczLayout layouts[] = {
            {"official", true, 0, 8, 72, false, true,
             8, 16, 24, 40, 56},
            {"legacy-count-after-magic", true, 8, 16, 64, true, false,
             0, 8, 16, 32, 48},
            {"legacy-section-list", false, 0, 0, 72, false, true,
             8, 16, 24, 40, 56},
        };

        bool waitingForMore = false;
        size_t headerSize = 0;
        uint64_t parsedOutputSize = 0;
        std::vector<NczSection> parsedSections;
        bool parsed = false;
        for (const auto& layout : layouts) {
            HeaderResult result =
                tryParseLayout(layout, headerSize, parsedOutputSize,
                               parsedSections);
            if (result == HeaderResult::Parsed) {
                parsed = true;
                break;
            }
            if (result == HeaderResult::NeedMore)
                waitingForMore = true;
        }
        if (!parsed) {
            if (waitingForMore)
                return true;
            error = "Invalid NCZ section header at 0x4000: " +
                    hexPreview(pending_.data() + ncaHeaderSize,
                               std::min<size_t>(
                                   32, pending_.size() - ncaHeaderSize));
            return false;
        }
        if (pending_.size() < headerSize + 8)
            return true;

        sections_ = std::move(parsedSections);
        outputSize_ = parsedOutputSize;

        blockMode_ = std::memcmp(pending_.data() + headerSize,
                                 "NCZBLOCK", 8) == 0;
        if (blockMode_) {
            if (pending_.size() < headerSize + 24)
                return true;
            uint8_t exponent = pending_[headerSize + 11];
            uint32_t countBlocks = read32(pending_.data() + headerSize + 12);
            uint64_t decompressed = read64(pending_.data() + headerSize + 16);
            if (exponent < 14 || exponent > 30 || countBlocks == 0 ||
                countBlocks > (1u << 24)) {
                error = "Invalid NCZBLOCK header.";
                return false;
            }
            uint64_t fullHeader = static_cast<uint64_t>(headerSize) + 24 +
                                  static_cast<uint64_t>(countBlocks) * 4;
            if (fullHeader > std::numeric_limits<size_t>::max())
                return false;
            if (pending_.size() < static_cast<size_t>(fullHeader))
                return true;
            blockSize_ = uint64_t{1} << exponent;
            if (decompressed != outputSize_ - ncaHeaderSize) {
                error = "NCZBLOCK decompressed size mismatch.";
                return false;
            }
            const uint8_t* sizes = pending_.data() + headerSize + 24;
            blockSizes_.reserve(countBlocks);
            for (uint32_t i = 0; i < countBlocks; ++i)
                blockSizes_.push_back(read32(sizes + i * 4));
            headerSize = static_cast<size_t>(fullHeader);
        } else {
            stream_ = ZSTD_createDStream();
            if (!stream_ || ZSTD_isError(ZSTD_initDStream(stream_))) {
                error = "Unable to initialize Zstandard decoder.";
                return false;
            }
        }

        uint64_t readyStartedUs = telemetry_enabled() ? now_us() : 0;
        bool ready = ready_(outputSize_);
        if (readyStartedUs) {
            uint64_t elapsedUs = now_us() - readyStartedUs;
            telemetryReadyUs_ += elapsedUs;
            telemetryTotalReadyUs_ += elapsedUs;
        }
        if (!ready) {
            error = "Installer rejected the NCZ output size.";
            return false;
        }
        uint64_t headerWriterStartedUs = telemetry_enabled() ? now_us() : 0;
        bool headerWritten = writer_(pending_.data(), ncaHeaderSize);
        if (headerWriterStartedUs) {
            uint64_t elapsedUs = now_us() - headerWriterStartedUs;
            telemetryWriterUs_ += elapsedUs;
            telemetryTotalWriterUs_ += elapsedUs;
            telemetryWriterCalls_++;
            telemetryTotalWriterCalls_++;
            telemetryWriterMaxUs_ = std::max(
                telemetryWriterMaxUs_, elapsedUs);
            telemetryTotalWriterMaxUs_ = std::max(
                telemetryTotalWriterMaxUs_, elapsedUs);
        }
        if (!headerWritten) {
            error = "Installer rejected the NCZ NCA header.";
            return false;
        }
        outputPosition_ = ncaHeaderSize;
        telemetryOutputBytes_ += ncaHeaderSize;
        telemetryTotalOutputBytes_ += ncaHeaderSize;
        pending_.consume(headerSize);
        headerReady_ = true;
        telemetry_log("decode", telemetryTag_.c_str(),
            "event=header mode=%s output_bytes=%llu block_bytes=%llu blocks=%zu",
            blockMode_ ? "block" : "solid",
            (unsigned long long)outputSize_,
            (unsigned long long)blockSize_, blockSizes_.size());
        return true;
    }

    bool emit(uint8_t* data, size_t size, std::string& error) {
        size_t done = 0;
        while (done < size) {
            while (currentSection_ < sections_.size() &&
                   outputPosition_ >= sections_[currentSection_].offset +
                                      sections_[currentSection_].size) {
                ++currentSection_;
            }
            if (currentSection_ >= sections_.size() ||
                outputPosition_ < sections_[currentSection_].offset) {
                error = "NCZ output does not map to a section.";
                return false;
            }
            const NczSection* section = &sections_[currentSection_];
            uint64_t remaining = section->offset + section->size -
                                 outputPosition_;
            size_t count = static_cast<size_t>(
                std::min<uint64_t>(remaining, size - done));
            uint64_t aesStartedUs = telemetry_enabled() ? now_us() : 0;
            bool transformed = AesCtr::transform(*section, outputPosition_,
                                                 data + done, count);
            if (aesStartedUs) {
                uint64_t elapsedUs = now_us() - aesStartedUs;
                telemetryAesUs_ += elapsedUs;
                telemetryTotalAesUs_ += elapsedUs;
            }
            if (!transformed) {
                error = "Unable to restore NCZ AES-CTR section.";
                return false;
            }
            uint64_t writerStartedUs = telemetry_enabled() ? now_us() : 0;
            bool written = writer_(data + done, count);
            if (writerStartedUs) {
                uint64_t elapsedUs = now_us() - writerStartedUs;
                telemetryWriterUs_ += elapsedUs;
                telemetryTotalWriterUs_ += elapsedUs;
                telemetryWriterCalls_++;
                telemetryTotalWriterCalls_++;
                telemetryWriterMaxUs_ = std::max(
                    telemetryWriterMaxUs_, elapsedUs);
                telemetryTotalWriterMaxUs_ = std::max(
                    telemetryTotalWriterMaxUs_, elapsedUs);
            }
            if (!written) {
                error = "Installer rejected decompressed NCZ data.";
                return false;
            }
            outputPosition_ += count;
            telemetryOutputBytes_ += count;
            telemetryTotalOutputBytes_ += count;
            done += count;
        }
        return true;
    }

    bool processSolid(std::string& error) {
        if (pending_.empty() || solidEnded_)
            return true;
        ZSTD_inBuffer input { pending_.data(), pending_.size(), 0 };
        while (input.pos < input.size) {
            ZSTD_outBuffer out { outputBuffer_.data(), outputBuffer_.size(), 0 };
            uint64_t zstdStartedUs = telemetry_enabled() ? now_us() : 0;
            size_t result = ZSTD_decompressStream(stream_, &out, &input);
            if (zstdStartedUs) {
                uint64_t elapsedUs = now_us() - zstdStartedUs;
                telemetryZstdUs_ += elapsedUs;
                telemetryTotalZstdUs_ += elapsedUs;
            }
            if (ZSTD_isError(result)) {
                error = ZSTD_getErrorName(result);
                return false;
            }
            if (out.pos && !emit(outputBuffer_.data(), out.pos, error))
                return false;
            if (result == 0)
                solidEnded_ = true;
            if (out.pos == 0 && input.pos == input.size)
                break;
        }
        solidConsumed_ += input.pos;
        pending_.consume(input.pos);
        return true;
    }

    bool processBlocks(std::string& error) {
        while (nextBlock_ < blockSizes_.size()) {
            size_t compressed = blockSizes_[nextBlock_];
            if (pending_.size() < compressed)
                return true;
            uint64_t remaining = outputSize_ - outputPosition_;
            size_t expected = static_cast<size_t>(
                std::min<uint64_t>(blockSize_, remaining));
            std::vector<uint8_t> output(expected);
            if (compressed == expected) {
                std::memcpy(output.data(), pending_.data(), expected);
            } else {
                uint64_t zstdStartedUs = telemetry_enabled() ? now_us() : 0;
                size_t result = ZSTD_decompress(output.data(), output.size(),
                                                pending_.data(), compressed);
                if (zstdStartedUs) {
                    uint64_t elapsedUs = now_us() - zstdStartedUs;
                    telemetryZstdUs_ += elapsedUs;
                    telemetryTotalZstdUs_ += elapsedUs;
                }
                if (ZSTD_isError(result) || result != expected) {
                    error = "Invalid compressed NCZ block.";
                    return false;
                }
            }
            if (!emit(output.data(), output.size(), error))
                return false;
            pending_.consume(compressed);
            ++nextBlock_;
        }
        return true;
    }

    void resetTelemetry(uint64_t now) {
        telemetryGeneration_ = telemetry_generation();
        telemetryLastMs_ = now;
        telemetryInputBytes_ = 0;
        telemetryOutputBytes_ = 0;
        telemetryZstdUs_ = 0;
        telemetryAesUs_ = 0;
        telemetryReadyUs_ = 0;
        telemetryWriterUs_ = 0;
        telemetryWriterMaxUs_ = 0;
        telemetryWriterCalls_ = 0;
    }

    void emitTelemetry(uint64_t now, bool force, bool summary) {
        uint32_t generation = telemetry_generation();
        if (!telemetry_enabled()) {
            if (telemetryGeneration_ != generation)
                resetTelemetry(now);
            return;
        }
        if (telemetryGeneration_ != generation) {
            telemetryStartMs_ = now;
            telemetryTotalInputBytes_ = 0;
            telemetryTotalOutputBytes_ = 0;
            telemetryTotalZstdUs_ = 0;
            telemetryTotalAesUs_ = 0;
            telemetryTotalReadyUs_ = 0;
            telemetryTotalWriterUs_ = 0;
            telemetryTotalWriterMaxUs_ = 0;
            telemetryTotalWriterCalls_ = 0;
            resetTelemetry(now);
            return;
        }
        uint64_t elapsedMs = summary
            ? now - telemetryStartMs_ : now - telemetryLastMs_;
        if (!force && elapsedMs < 5000)
            return;
        if (!elapsedMs)
            elapsedMs = 1;
        uint64_t inputBytes = summary
            ? telemetryTotalInputBytes_ : telemetryInputBytes_;
        uint64_t outputBytes = summary
            ? telemetryTotalOutputBytes_ : telemetryOutputBytes_;
        uint64_t zstdUs = summary ? telemetryTotalZstdUs_ : telemetryZstdUs_;
        uint64_t aesUs = summary ? telemetryTotalAesUs_ : telemetryAesUs_;
        uint64_t readyUs = summary ? telemetryTotalReadyUs_ : telemetryReadyUs_;
        uint64_t writerUs = summary ? telemetryTotalWriterUs_ : telemetryWriterUs_;
        uint64_t writerMaxUs = summary
            ? telemetryTotalWriterMaxUs_ : telemetryWriterMaxUs_;
        uint32_t writerCalls = summary
            ? telemetryTotalWriterCalls_ : telemetryWriterCalls_;
        telemetry_log("decode", telemetryTag_.c_str(),
            "event=%s interval_ms=%llu mode=%s input_bytes=%llu output_bytes=%llu "
            "input_bps=%llu output_bps=%llu ratio_permille=%llu pending_bytes=%zu "
            "zstd_us=%llu aes_us=%llu ready_us=%llu writer_us=%llu "
            "writer_calls=%u writer_max_us=%llu",
            summary ? "summary" : "interval",
            (unsigned long long)elapsedMs,
            blockMode_ ? "block" : (headerReady_ ? "solid" : "header"),
            (unsigned long long)inputBytes,
            (unsigned long long)outputBytes,
            (unsigned long long)(inputBytes * 1000 / elapsedMs),
            (unsigned long long)(outputBytes * 1000 / elapsedMs),
            (unsigned long long)(inputBytes
                ? outputBytes * 1000 / inputBytes : 0),
            pending_.size(), (unsigned long long)zstdUs,
            (unsigned long long)aesUs, (unsigned long long)readyUs,
            (unsigned long long)writerUs, writerCalls,
            (unsigned long long)writerMaxUs);
        if (!summary)
            resetTelemetry(now);
    }

    uint64_t inputSize_;
    uint64_t inputRemaining_;
    Ready ready_;
    Writer writer_;
    std::string telemetryTag_;
    ByteQueue pending_;
    std::vector<NczSection> sections_;
    std::vector<uint32_t> blockSizes_;
    std::vector<uint8_t> outputBuffer_ =
        std::vector<uint8_t>(4 * 1024 * 1024);
    ZSTD_DStream* stream_ = nullptr;
    uint64_t outputSize_ = 0;
    uint64_t outputPosition_ = 0;
    uint64_t blockSize_ = 0;
    size_t currentSection_ = 0;
    size_t nextBlock_ = 0;
    uint64_t solidConsumed_ = 0;
    bool headerReady_ = false;
    bool blockMode_ = false;
    bool solidEnded_ = false;
    uint32_t telemetryGeneration_ = 0;
    uint64_t telemetryStartMs_ = 0;
    uint64_t telemetryLastMs_ = 0;
    uint64_t telemetryInputBytes_ = 0;
    uint64_t telemetryOutputBytes_ = 0;
    uint64_t telemetryZstdUs_ = 0;
    uint64_t telemetryAesUs_ = 0;
    uint64_t telemetryReadyUs_ = 0;
    uint64_t telemetryWriterUs_ = 0;
    uint64_t telemetryWriterMaxUs_ = 0;
    uint32_t telemetryWriterCalls_ = 0;
    uint64_t telemetryTotalInputBytes_ = 0;
    uint64_t telemetryTotalOutputBytes_ = 0;
    uint64_t telemetryTotalZstdUs_ = 0;
    uint64_t telemetryTotalAesUs_ = 0;
    uint64_t telemetryTotalReadyUs_ = 0;
    uint64_t telemetryTotalWriterUs_ = 0;
    uint64_t telemetryTotalWriterMaxUs_ = 0;
    uint32_t telemetryTotalWriterCalls_ = 0;
};

} // namespace

class PackageStream::Impl {
public:
    Impl(bool compressed, PackageCallbacks callbacks, std::string telemetryTag)
        : compressed_(compressed), callbacks_(std::move(callbacks)),
          telemetryTag_(std::move(telemetryTag)) {}

    bool write(const uint8_t* data, size_t size) {
        if (failed_ || finished_ || (!data && size)) {
            if (error_.empty())
                error_ = "Invalid package stream state.";
            return false;
        }
        consumed_ += size;
        pending_.append(data, size);
        if (!headerReady_ && !parseHeader())
            return false;
        if (!headerReady_)
            return true;
        return process();
    }

    bool finish() {
        if (failed_ || finished_)
            return false;
        if (!headerReady_ && !parseHeader()) {
            if (error_.empty())
                error_ = "Incomplete PFS0 header.";
            return false;
        }
        if (!process())
            return false;
        if (currentDecoder_) {
            if (!currentDecoder_->finish(error_))
                return fail();
            currentDecoder_.reset();
        }
        if (fileOpen_ && !endCurrentFile())
            return false;
        if (entryIndex_ != entries_.size() || !pending_.empty()) {
            error_ = "PFS0 stream ended before all files were processed.";
            return fail();
        }
        finished_ = true;
        return true;
    }

    uint64_t consumed() const { return consumed_; }
    const std::string& error() const { return error_; }

    bool checkpoint(PackageStreamState& out) const {
        if (failed_ || finished_ || !headerReady_)
            return false;
        uint64_t decoderPending = 0;
        if (currentDecoder_) {
            if (!currentDecoder_->checkpointable())
                return false;
            decoderPending = currentDecoder_->pendingSize();
        }
        // Undigested bytes roll back: pending_ was never handed to a parser,
        // decoder pending was counted into the entry position but produced
        // no output yet.
        if (decoderPending > dataPosition_ ||
            decoderPending > std::numeric_limits<uint64_t>::max() -
                                 currentInputRemaining_)
            return false;
        out = PackageStreamState {};
        out.consumed = consumed_ - pending_.size() - decoderPending;
        out.headerSize = headerSize_;
        out.dataPosition = dataPosition_ - decoderPending;
        out.currentInputRemaining = currentInputRemaining_ + decoderPending;
        out.entryIndex = static_cast<uint32_t>(entryIndex_);
        out.fileOpen = fileOpen_;
        out.skipped = currentSkipped_;
        out.entries.reserve(entries_.size());
        for (const auto& entry : entries_)
            out.entries.push_back({entry.offset, entry.size, entry.name});
        if (currentDecoder_)
            currentDecoder_->exportState(out);
        return true;
    }

    bool restore(const PackageStreamState& state) {
        if (failed_ || finished_ || consumed_ != 0 || headerReady_) {
            error_ = "Package stream restore requires a fresh stream.";
            return fail();
        }
        if (!validateRestore(state)) {
            if (error_.empty())
                error_ = "Package stream checkpoint is inconsistent.";
            return fail();
        }
        entries_.clear();
        entries_.reserve(state.entries.size());
        for (const auto& entry : state.entries) {
            PfsEntry parsed;
            parsed.offset = entry.offset;
            parsed.size = entry.size;
            parsed.name = entry.name;
            entries_.push_back(std::move(parsed));
        }
        headerSize_ = state.headerSize;
        dataPosition_ = state.dataPosition;
        currentInputRemaining_ = state.currentInputRemaining;
        entryIndex_ = state.entryIndex;
        fileOpen_ = state.fileOpen;
        currentSkipped_ = state.skipped;
        consumed_ = state.consumed;
        headerReady_ = true;
        if (fileOpen_)
            currentName_ = entries_[entryIndex_].name;
        if (state.decoderPresent) {
            currentDecoder_ = std::make_unique<NczDecoder>(
                entries_[entryIndex_].size, telemetryTag_,
                [this](uint64_t size) {
                    return callbacks_.setFileSize &&
                           callbacks_.setFileSize(size);
                },
                [this](const uint8_t* data, size_t size) {
                    return callbacks_.writeFile &&
                           callbacks_.writeFile(data, size);
                });
            if (!currentDecoder_->restore(state, currentInputRemaining_,
                                          error_))
                return fail();
        }
        telemetry_log("package", telemetryTag_.c_str(),
            "event=restore consumed=%llu entry=%u open=%d decoder=%d",
            (unsigned long long)consumed_, state.entryIndex,
            fileOpen_ ? 1 : 0, state.decoderPresent ? 1 : 0);
        return true;
    }

private:
    bool validateRestore(const PackageStreamState& state) const {
        if (state.entries.empty() || state.entries.size() > 4096 ||
            state.entryIndex > state.entries.size() || state.headerSize < 16)
            return false;
        uint64_t previousEnd = 0;
        for (const auto& entry : state.entries) {
            if (entry.name.empty() || entry.offset < previousEnd ||
                entry.size > std::numeric_limits<uint64_t>::max() -
                                 entry.offset)
                return false;
            previousEnd = entry.offset + entry.size;
        }
        if (state.consumed != state.headerSize + state.dataPosition)
            return false;
        if (state.fileOpen) {
            if (state.entryIndex >= state.entries.size())
                return false;
            const auto& entry = state.entries[state.entryIndex];
            if (state.currentInputRemaining == 0 ||
                state.currentInputRemaining > entry.size)
                return false;
            if (state.dataPosition !=
                entry.offset + (entry.size - state.currentInputRemaining))
                return false;
            bool ncz = compressed_ && entry.name.size() >= 4 &&
                       entry.name.substr(entry.name.size() - 4) == ".ncz";
            bool wantsDecoder = ncz && !state.skipped;
            if (state.decoderPresent != wantsDecoder)
                return false;
        } else {
            if (state.currentInputRemaining != 0 || state.decoderPresent ||
                state.skipped)
                return false;
            if (state.entryIndex < state.entries.size() &&
                state.dataPosition > state.entries[state.entryIndex].offset)
                return false;
            if (state.entryIndex == state.entries.size() &&
                state.dataPosition < previousEnd)
                return false;
        }
        return true;
    }

    bool fail() {
        failed_ = true;
        return false;
    }

    bool parseHeader() {
        if (pending_.size() < 16)
            return true;
        uint64_t parseStartedUs = telemetry_enabled() ? now_us() : 0;
        if (std::memcmp(pending_.data(), "PFS0", 4) != 0) {
            error_ = "Package is not a PFS0 NSP/NSZ.";
            return fail();
        }
        uint32_t count = read32(pending_.data() + 4);
        uint32_t stringsSize = read32(pending_.data() + 8);
        if (count == 0 || count > 4096 || stringsSize > 16 * 1024 * 1024) {
            error_ = "Invalid PFS0 header values.";
            return fail();
        }
        uint64_t header64 = 16 + static_cast<uint64_t>(count) * 24 +
                            stringsSize;
        if (header64 > std::numeric_limits<size_t>::max()) {
            error_ = "PFS0 header is too large.";
            return fail();
        }
        size_t header = static_cast<size_t>(header64);
        if (pending_.size() < header)
            return true;
        const uint8_t* table = pending_.data() + 16 + count * 24;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* entry = pending_.data() + 16 + i * 24;
            uint32_t nameOffset = read32(entry + 16);
            if (nameOffset >= stringsSize) {
                error_ = "Invalid PFS0 string-table offset.";
                return fail();
            }
            const char* name = reinterpret_cast<const char*>(table + nameOffset);
            size_t available = stringsSize - nameOffset;
            const void* end = std::memchr(name, '\0', available);
            if (!end) {
                error_ = "Unterminated PFS0 filename.";
                return fail();
            }
            PfsEntry parsed;
            parsed.offset = read64(entry);
            parsed.size = read64(entry + 8);
            parsed.name.assign(name, static_cast<const char*>(end) - name);
            entries_.push_back(std::move(parsed));
        }
        std::sort(entries_.begin(), entries_.end(),
                  [](const PfsEntry& a, const PfsEntry& b) {
                      return a.offset < b.offset;
                  });
        uint64_t previousEnd = 0;
        for (const auto& entry : entries_) {
            if (entry.offset < previousEnd ||
                entry.size > std::numeric_limits<uint64_t>::max() -
                                 entry.offset) {
                error_ = "PFS0 file ranges overlap or overflow.";
                return fail();
            }
            previousEnd = entry.offset + entry.size;
        }
        headerSize_ = header;
        dataPosition_ = 0;
        pending_.consume(header);
        headerReady_ = true;
        telemetry_log("package", telemetryTag_.c_str(),
            "event=header entries=%u header_bytes=%zu parse_us=%llu",
            count, header,
            (unsigned long long)(parseStartedUs ? now_us() - parseStartedUs : 0));
        return true;
    }

    bool beginCurrentFile() {
        const PfsEntry& entry = entries_[entryIndex_];
        currentInputRemaining_ = entry.size;
        currentName_ = entry.name;
        bool ncz = compressed_ && currentName_.size() >= 4 &&
                   currentName_.substr(currentName_.size() - 4) == ".ncz";
        uint64_t announcedSize = ncz ? 0 : entry.size;
        std::string announcedName = currentName_;
        if (ncz)
            announcedName.replace(announcedName.size() - 4, 4, ".nca");
        // Unwanted entries (delta fragments, PERF_PLAN 3.4) are dropped
        // whole: their bytes are consumed raw below, with no NCZ decode,
        // AES, hashing or installer callbacks.
        if (callbacks_.skipFile && callbacks_.skipFile(announcedName)) {
            currentSkipped_ = true;
            fileOpen_ = true;
            log_msg("[install] skipping package entry '%s' bytes=%llu\n",
                    currentName_.c_str(),
                    static_cast<unsigned long long>(entry.size));
            telemetry_log("package", telemetryTag_.c_str(),
                "event=skip name=%s bytes=%llu", currentName_.c_str(),
                (unsigned long long)entry.size);
            return true;
        }
        if (!callbacks_.beginFile ||
            !callbacks_.beginFile(announcedName, announcedSize)) {
            error_ = "Installer rejected PFS0 file " + entry.name;
            return fail();
        }
        fileOpen_ = true;
        if (ncz) {
            currentDecoder_ = std::make_unique<NczDecoder>(
                entry.size, telemetryTag_,
                [this](uint64_t size) {
                    return callbacks_.setFileSize &&
                           callbacks_.setFileSize(size);
                },
                [this](const uint8_t* data, size_t size) {
                    return callbacks_.writeFile &&
                           callbacks_.writeFile(data, size);
                });
        }
        return true;
    }

    bool endCurrentFile() {
        if (currentDecoder_) {
            if (!currentDecoder_->finish(error_))
                return fail();
            currentDecoder_.reset();
        }
        if (currentSkipped_) {
            currentSkipped_ = false;
        } else if (!callbacks_.endFile || !callbacks_.endFile()) {
            error_ = "Installer failed to finalize PFS0 file " + currentName_;
            return fail();
        }
        fileOpen_ = false;
        currentName_.clear();
        ++entryIndex_;
        return true;
    }

    bool process() {
        while (entryIndex_ < entries_.size()) {
            const PfsEntry& entry = entries_[entryIndex_];
            if (!fileOpen_) {
                if (dataPosition_ < entry.offset) {
                    uint64_t skip64 = entry.offset - dataPosition_;
                    size_t skip = static_cast<size_t>(
                        std::min<uint64_t>(skip64, pending_.size()));
                    pending_.consume(skip);
                    dataPosition_ += skip;
                    if (dataPosition_ < entry.offset)
                        return true;
                }
                if (!beginCurrentFile())
                    return false;
                if (currentInputRemaining_ == 0 && !endCurrentFile())
                    return false;
            }
            if (pending_.empty())
                return true;
            size_t count = static_cast<size_t>(
                std::min<uint64_t>(currentInputRemaining_, pending_.size()));
            bool ok;
            if (currentSkipped_)
                ok = true;
            else if (currentDecoder_)
                ok = currentDecoder_->write(pending_.data(), count, error_);
            else
                ok = callbacks_.writeFile &&
                     callbacks_.writeFile(pending_.data(), count);
            if (!ok) {
                if (error_.empty())
                    error_ = "Installer failed while consuming " + currentName_;
                return fail();
            }
            pending_.consume(count);
            dataPosition_ += count;
            currentInputRemaining_ -= count;
            if (currentInputRemaining_ == 0 && !endCurrentFile())
                return false;
        }
        return true;
    }

    bool compressed_;
    PackageCallbacks callbacks_;
    std::string telemetryTag_;
    ByteQueue pending_;
    std::vector<PfsEntry> entries_;
    std::unique_ptr<NczDecoder> currentDecoder_;
    std::string currentName_;
    std::string error_;
    uint64_t consumed_ = 0;
    uint64_t headerSize_ = 0;
    uint64_t dataPosition_ = 0;
    uint64_t currentInputRemaining_ = 0;
    size_t entryIndex_ = 0;
    bool headerReady_ = false;
    bool fileOpen_ = false;
    bool currentSkipped_ = false;
    bool finished_ = false;
    bool failed_ = false;
};

PackageStream::PackageStream(bool compressed, PackageCallbacks callbacks,
                             std::string telemetryTag)
    : impl_(std::make_unique<Impl>(compressed, std::move(callbacks),
                                   std::move(telemetryTag))) {}

PackageStream::~PackageStream() = default;

bool PackageStream::write(const uint8_t* data, size_t size) {
    return impl_->write(data, size);
}

bool PackageStream::finish() {
    return impl_->finish();
}

uint64_t PackageStream::consumed() const {
    return impl_->consumed();
}

const std::string& PackageStream::error() const {
    return impl_->error();
}

bool PackageStream::checkpoint(PackageStreamState& out) const {
    return impl_->checkpoint(out);
}

bool PackageStream::restore(const PackageStreamState& state) {
    return impl_->restore(state);
}

} // namespace pipensx::install
