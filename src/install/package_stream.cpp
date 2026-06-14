#include "package_stream.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <zstd.h>

#ifdef __SWITCH__
#include <mbedtls/aes.h>
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

struct PfsEntry {
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string name;
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
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        if (mbedtls_aes_setkey_enc(&aes, section.key.data(), 128) != 0) {
            mbedtls_aes_free(&aes);
            return false;
        }
#else
        AES_KEY aes;
        if (AES_set_encrypt_key(section.key.data(), 128, &aes) != 0)
            return false;
#endif
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
#ifdef __SWITCH__
            if (mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                                      counter, stream) != 0) {
                mbedtls_aes_free(&aes);
                return false;
            }
#else
            AES_encrypt(counter, stream, &aes);
#endif
            size_t count = std::min<size_t>(16 - skip, size - done);
            for (size_t i = 0; i < count; ++i)
                data[done + i] ^= stream[skip + i];
            done += count;
        }
#ifdef __SWITCH__
        mbedtls_aes_free(&aes);
#endif
        return true;
    }
};

class NczDecoder {
public:
    using Ready = std::function<bool(uint64_t)>;
    using Writer = std::function<bool(const uint8_t*, size_t)>;

    NczDecoder(uint64_t inputSize, Ready ready, Writer writer)
        : inputRemaining_(inputSize), ready_(std::move(ready)),
          writer_(std::move(writer)) {}

    ~NczDecoder() {
        if (stream_)
            ZSTD_freeDStream(stream_);
    }

    bool write(const uint8_t* data, size_t size, std::string& error) {
        if (size > inputRemaining_) {
            error = "NCZ input exceeds its PFS0 entry.";
            return false;
        }
        inputRemaining_ -= size;
        pending_.insert(pending_.end(), data, data + size);
        if (!headerReady_ && !parseHeader(error))
            return error.empty();
        if (!headerReady_)
            return true;
        return blockMode_ ? processBlocks(error) : processSolid(error);
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
    bool parseHeader(std::string& error) {
        constexpr size_t ncaHeaderSize = 0x4000;
        if (pending_.size() < ncaHeaderSize + 16)
            return true;
        if (std::memcmp(pending_.data() + ncaHeaderSize, "NCZSECTN", 8) != 0) {
            error = "NCZSECTN header is missing.";
            return false;
        }
        uint64_t count = read64(pending_.data() + ncaHeaderSize + 8);
        if (count == 0 || count > 64) {
            error = "Invalid NCZ section count.";
            return false;
        }
        uint64_t headerSize64 = ncaHeaderSize + 16 + count * 64;
        if (headerSize64 > std::numeric_limits<size_t>::max()) {
            error = "NCZ header is too large.";
            return false;
        }
        size_t headerSize = static_cast<size_t>(headerSize64);
        if (pending_.size() < headerSize + 8)
            return true;

        sections_.reserve(static_cast<size_t>(count));
        const uint8_t* p = pending_.data() + ncaHeaderSize + 16;
        outputSize_ = ncaHeaderSize;
        uint64_t previousEnd = ncaHeaderSize;
        for (uint64_t i = 0; i < count; ++i, p += 64) {
            NczSection section;
            section.offset = read64(p);
            section.size = read64(p + 8);
            section.cryptoType = read64(p + 16);
            std::memcpy(section.key.data(), p + 32, 16);
            std::memcpy(section.counter.data(), p + 48, 16);
            if (section.offset < previousEnd ||
                section.size > std::numeric_limits<uint64_t>::max() -
                                   section.offset) {
                error = "NCZ output size overflows.";
                return false;
            }
            if (section.offset > previousEnd) {
                NczSection gap;
                gap.offset = previousEnd;
                gap.size = section.offset - previousEnd;
                sections_.push_back(gap);
            }
            sections_.push_back(section);
            previousEnd = section.offset + section.size;
            outputSize_ = std::max(outputSize_, previousEnd);
        }

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
            uint64_t fullHeader = headerSize64 + 24 +
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

        if (!ready_(outputSize_)) {
            error = "Installer rejected the NCZ output size.";
            return false;
        }
        if (!writer_(pending_.data(), ncaHeaderSize)) {
            error = "Installer rejected the NCZ NCA header.";
            return false;
        }
        outputPosition_ = ncaHeaderSize;
        pending_.erase(pending_.begin(), pending_.begin() + headerSize);
        headerReady_ = true;
        return true;
    }

    bool emit(uint8_t* data, size_t size, std::string& error) {
        size_t done = 0;
        while (done < size) {
            const NczSection* section = nullptr;
            for (const auto& candidate : sections_) {
                if (outputPosition_ >= candidate.offset &&
                    outputPosition_ < candidate.offset + candidate.size) {
                    section = &candidate;
                    break;
                }
            }
            if (!section) {
                error = "NCZ output does not map to a section.";
                return false;
            }
            uint64_t remaining = section->offset + section->size -
                                 outputPosition_;
            size_t count = static_cast<size_t>(
                std::min<uint64_t>(remaining, size - done));
            if (!AesCtr::transform(*section, outputPosition_,
                                   data + done, count)) {
                error = "Unable to restore NCZ AES-CTR section.";
                return false;
            }
            if (!writer_(data + done, count)) {
                error = "Installer rejected decompressed NCZ data.";
                return false;
            }
            outputPosition_ += count;
            done += count;
        }
        return true;
    }

    bool processSolid(std::string& error) {
        if (pending_.empty() || solidEnded_)
            return true;
        ZSTD_inBuffer input { pending_.data(), pending_.size(), 0 };
        std::array<uint8_t, 256 * 1024> output {};
        while (input.pos < input.size) {
            ZSTD_outBuffer out { output.data(), output.size(), 0 };
            size_t result = ZSTD_decompressStream(stream_, &out, &input);
            if (ZSTD_isError(result)) {
                error = ZSTD_getErrorName(result);
                return false;
            }
            if (out.pos && !emit(output.data(), out.pos, error))
                return false;
            if (result == 0)
                solidEnded_ = true;
            if (out.pos == 0 && input.pos == input.size)
                break;
        }
        pending_.erase(pending_.begin(), pending_.begin() + input.pos);
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
                size_t result = ZSTD_decompress(output.data(), output.size(),
                                                pending_.data(), compressed);
                if (ZSTD_isError(result) || result != expected) {
                    error = "Invalid compressed NCZ block.";
                    return false;
                }
            }
            if (!emit(output.data(), output.size(), error))
                return false;
            pending_.erase(pending_.begin(), pending_.begin() + compressed);
            ++nextBlock_;
        }
        return true;
    }

    uint64_t inputRemaining_;
    Ready ready_;
    Writer writer_;
    std::vector<uint8_t> pending_;
    std::vector<NczSection> sections_;
    std::vector<uint32_t> blockSizes_;
    ZSTD_DStream* stream_ = nullptr;
    uint64_t outputSize_ = 0;
    uint64_t outputPosition_ = 0;
    uint64_t blockSize_ = 0;
    size_t nextBlock_ = 0;
    bool headerReady_ = false;
    bool blockMode_ = false;
    bool solidEnded_ = false;
};

} // namespace

class PackageStream::Impl {
public:
    Impl(bool compressed, PackageCallbacks callbacks)
        : compressed_(compressed), callbacks_(std::move(callbacks)) {}

    bool write(const uint8_t* data, size_t size) {
        if (failed_ || finished_ || (!data && size)) {
            if (error_.empty())
                error_ = "Invalid package stream state.";
            return false;
        }
        consumed_ += size;
        pending_.insert(pending_.end(), data, data + size);
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

private:
    bool fail() {
        failed_ = true;
        return false;
    }

    bool parseHeader() {
        if (pending_.size() < 16)
            return true;
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
        pending_.erase(pending_.begin(), pending_.begin() + header);
        headerReady_ = true;
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
        if (!callbacks_.beginFile ||
            !callbacks_.beginFile(announcedName, announcedSize)) {
            error_ = "Installer rejected PFS0 file " + entry.name;
            return fail();
        }
        fileOpen_ = true;
        if (ncz) {
            currentDecoder_ = std::make_unique<NczDecoder>(
                entry.size,
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
        if (!callbacks_.endFile || !callbacks_.endFile()) {
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
                    pending_.erase(pending_.begin(), pending_.begin() + skip);
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
            if (currentDecoder_)
                ok = currentDecoder_->write(pending_.data(), count, error_);
            else
                ok = callbacks_.writeFile &&
                     callbacks_.writeFile(pending_.data(), count);
            if (!ok) {
                if (error_.empty())
                    error_ = "Installer failed while consuming " + currentName_;
                return fail();
            }
            pending_.erase(pending_.begin(), pending_.begin() + count);
            dataPosition_ += count;
            currentInputRemaining_ -= count;
            if (currentInputRemaining_ == 0 && !endCurrentFile())
                return false;
        }
        return true;
    }

    bool compressed_;
    PackageCallbacks callbacks_;
    std::vector<uint8_t> pending_;
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
    bool finished_ = false;
    bool failed_ = false;
};

PackageStream::PackageStream(bool compressed, PackageCallbacks callbacks)
    : impl_(std::make_unique<Impl>(compressed, std::move(callbacks))) {}

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

} // namespace pipensx::install
