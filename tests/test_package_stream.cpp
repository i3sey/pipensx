#include "install/package_stream.hpp"

#include <zstd.h>
#include <openssl/aes.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

using pipensx::install::PackageCallbacks;
using pipensx::install::PackageStream;

namespace {

void append32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void append64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

enum class NczTestLayout {
    LegacyCountAfterMagic,
    OfficialCountBeforeSections,
    LegacySectionList,
};

std::vector<uint8_t> makePfs0(
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> strings;
    std::vector<uint32_t> nameOffsets;
    for (const auto& file : files) {
        nameOffsets.push_back(static_cast<uint32_t>(strings.size()));
        strings.insert(strings.end(), file.first.begin(), file.first.end());
        strings.push_back(0);
    }

    std::vector<uint8_t> out {'P', 'F', 'S', '0'};
    append32(out, static_cast<uint32_t>(files.size()));
    append32(out, static_cast<uint32_t>(strings.size()));
    append32(out, 0);
    uint64_t offset = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        append64(out, offset);
        append64(out, files[i].second.size());
        append32(out, nameOffsets[i]);
        append32(out, 0);
        offset += files[i].second.size();
    }
    out.insert(out.end(), strings.begin(), strings.end());
    for (const auto& file : files)
        out.insert(out.end(), file.second.begin(), file.second.end());
    return out;
}

void cryptCtr(std::vector<uint8_t>& data, uint64_t absoluteOffset,
              const std::array<uint8_t, 16>& key,
              const std::array<uint8_t, 16>& baseCounter) {
    AES_KEY aes;
    assert(AES_set_encrypt_key(key.data(), 128, &aes) == 0);
    size_t done = 0;
    while (done < data.size()) {
        uint64_t position = absoluteOffset + done;
        uint64_t block = position >> 4;
        size_t skip = static_cast<size_t>(position & 15);
        uint8_t counter[16] {};
        std::memcpy(counter, baseCounter.data(), 8);
        for (unsigned i = 0; i < 8; ++i)
            counter[15 - i] = static_cast<uint8_t>(block >> (i * 8));
        uint8_t stream[16];
        AES_encrypt(counter, stream, &aes);
        size_t count = std::min<size_t>(16 - skip, data.size() - done);
        for (size_t i = 0; i < count; ++i)
            data[done + i] ^= stream[skip + i];
        done += count;
    }
}

void appendNczSection(std::vector<uint8_t>& out, NczTestLayout layout,
                      uint64_t offset, uint64_t size, uint64_t cryptoType,
                      const std::array<uint8_t, 16>& key,
                      const std::array<uint8_t, 16>& counter,
                      uint64_t reserved = 0) {
    if (layout != NczTestLayout::LegacyCountAfterMagic)
        out.insert(out.end(), {'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'});
    append64(out, offset);
    append64(out, size);
    append64(out, cryptoType);
    append64(out, reserved);
    out.insert(out.end(), key.begin(), key.end());
    out.insert(out.end(), counter.begin(), counter.end());
}

std::vector<uint8_t> makeSolidNcz(const std::vector<uint8_t>& nca,
                                  bool encrypted = false,
                                  NczTestLayout layout =
                                      NczTestLayout::LegacyCountAfterMagic,
                                  uint64_t cryptoTypeOverride =
                                      std::numeric_limits<uint64_t>::max(),
                                  uint64_t reserved = 0) {
    assert(nca.size() > 0x4000);
    const uint64_t bodySize = nca.size() - 0x4000;
    std::array<uint8_t, 16> key {};
    std::array<uint8_t, 16> counter {};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i * 13 + 7);
        counter[i] = static_cast<uint8_t>(i * 5 + 3);
    }
    std::vector<uint8_t> body(nca.begin() + 0x4000, nca.end());
    if (encrypted)
        cryptCtr(body, 0x4000, key, counter);
    size_t bound = ZSTD_compressBound(bodySize);
    std::vector<uint8_t> compressed(bound);
    size_t compressedSize = ZSTD_compress(
        compressed.data(), compressed.size(), body.data(), bodySize, 3);
    assert(!ZSTD_isError(compressedSize));
    compressed.resize(compressedSize);

    std::vector<uint8_t> out(nca.begin(), nca.begin() + 0x4000);
    if (layout == NczTestLayout::OfficialCountBeforeSections)
        append64(out, 1);
    if (layout == NczTestLayout::LegacyCountAfterMagic) {
        out.insert(out.end(), {'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'});
        append64(out, 1);
    }
    uint64_t cryptoType = cryptoTypeOverride;
    if (cryptoType == std::numeric_limits<uint64_t>::max())
        cryptoType = encrypted ? 3 : 1;
    appendNczSection(out, layout, 0x4000, bodySize, cryptoType,
                     key, counter, reserved);
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

std::vector<uint8_t> makeSolidNczWithSections(
    const std::vector<uint8_t>& nca,
    const std::vector<std::pair<uint64_t, uint64_t>>& sections) {
    assert(nca.size() > 0x4000);
    const uint64_t bodySize = nca.size() - 0x4000;
    size_t bound = ZSTD_compressBound(bodySize);
    std::vector<uint8_t> compressed(bound);
    size_t compressedSize = ZSTD_compress(
        compressed.data(), compressed.size(), nca.data() + 0x4000,
        bodySize, 3);
    assert(!ZSTD_isError(compressedSize));
    compressed.resize(compressedSize);

    std::array<uint8_t, 16> empty {};
    std::vector<uint8_t> out(nca.begin(), nca.begin() + 0x4000);
    out.insert(out.end(), {'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'});
    append64(out, sections.size());
    for (const auto& section : sections) {
        appendNczSection(out, NczTestLayout::LegacyCountAfterMagic,
                         section.first, section.second, 1, empty, empty);
    }
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

std::vector<uint8_t> makeBlockNcz(
    const std::vector<uint8_t>& nca,
    NczTestLayout layout = NczTestLayout::LegacyCountAfterMagic,
    bool encrypted = false) {
    constexpr uint8_t exponent = 16;
    constexpr size_t blockSize = size_t{1} << exponent;
    const size_t bodySize = nca.size() - 0x4000;
    std::array<uint8_t, 16> key {};
    std::array<uint8_t, 16> counter {};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i * 13 + 7);
        counter[i] = static_cast<uint8_t>(i * 5 + 3);
    }
    std::vector<uint8_t> body(nca.begin() + 0x4000, nca.end());
    if (encrypted)
        cryptCtr(body, 0x4000, key, counter);
    std::vector<std::vector<uint8_t>> blocks;
    std::vector<uint32_t> sizes;
    for (size_t offset = 0; offset < bodySize; offset += blockSize) {
        size_t expected = std::min(blockSize, bodySize - offset);
        std::vector<uint8_t> compressed(ZSTD_compressBound(expected));
        size_t result = ZSTD_compress(compressed.data(), compressed.size(),
                                      body.data() + offset,
                                      expected, 3);
        assert(!ZSTD_isError(result));
        if (result >= expected) {
            compressed.assign(body.begin() + offset,
                              body.begin() + offset + expected);
        } else {
            compressed.resize(result);
        }
        sizes.push_back(static_cast<uint32_t>(compressed.size()));
        blocks.push_back(std::move(compressed));
    }

    std::vector<uint8_t> out(nca.begin(), nca.begin() + 0x4000);
    if (layout == NczTestLayout::OfficialCountBeforeSections)
        append64(out, 1);
    if (layout == NczTestLayout::LegacyCountAfterMagic) {
        out.insert(out.end(), {'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'});
        append64(out, 1);
    }
    std::array<uint8_t, 16> empty {};
    appendNczSection(out, layout, 0x4000, bodySize, encrypted ? 3 : 1,
                     encrypted ? key : empty, encrypted ? counter : empty);
    out.insert(out.end(), {'N', 'C', 'Z', 'B', 'L', 'O', 'C', 'K'});
    out.insert(out.end(), {0, 0, 0, exponent});
    append32(out, static_cast<uint32_t>(blocks.size()));
    append64(out, bodySize);
    for (uint32_t size : sizes)
        append32(out, size);
    for (const auto& block : blocks)
        out.insert(out.end(), block.begin(), block.end());
    return out;
}

struct Capture {
    std::vector<std::string> names;
    std::vector<std::vector<uint8_t>> files;
    std::vector<size_t> writeSizes;
    uint64_t expected = 0;
    bool open = false;

    PackageCallbacks callbacks() {
        PackageCallbacks cb;
        cb.beginFile = [this](const std::string& name, uint64_t size) {
            assert(!open);
            names.push_back(name);
            files.emplace_back();
            expected = size;
            open = true;
            return true;
        };
        cb.setFileSize = [this](uint64_t size) {
            assert(open && expected == 0);
            expected = size;
            return true;
        };
        cb.writeFile = [this](const uint8_t* data, size_t size) {
            assert(open);
            writeSizes.push_back(size);
            files.back().insert(files.back().end(), data, data + size);
            return true;
        };
        cb.endFile = [this] {
            assert(open && files.back().size() == expected);
            open = false;
            return true;
        };
        return cb;
    }
};

void feed(PackageStream& stream, const std::vector<uint8_t>& data) {
    static const size_t chunks[] = {1, 7, 31, 4093, 17, 65537};
    size_t offset = 0;
    size_t index = 0;
    while (offset < data.size()) {
        size_t count = std::min(chunks[index++ % 6], data.size() - offset);
        if (!stream.write(data.data() + offset, count)) {
            std::cerr << stream.error() << '\n';
            assert(false);
        }
        offset += count;
    }
    if (!stream.finish()) {
        std::cerr << stream.error() << '\n';
        assert(false);
    }
}

void testNsp() {
    std::vector<uint8_t> first {1, 2, 3, 4, 5};
    std::vector<uint8_t> second(100000);
    for (size_t i = 0; i < second.size(); ++i)
        second[i] = static_cast<uint8_t>(i * 17);
    auto package = makePfs0({{"first.nca", first}, {"second.nca", second}});
    Capture capture;
    PackageStream stream(false, capture.callbacks());
    feed(stream, package);
    assert((capture.names == std::vector<std::string>{
        "first.nca", "second.nca"}));
    assert(capture.files[0] == first);
    assert(capture.files[1] == second);
}

void testNsz() {
    std::vector<uint8_t> nca(0x4000 + 200000);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 29) ^ (i >> 8));
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNcz(nca)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.names.size() == 1);
    assert(capture.names[0] == "00112233445566778899aabbccddeeff.nca");
    assert(capture.files[0] == nca);
}

void testNszUsesFourMiBOutputChunks() {
    constexpr size_t outputChunk = 4 * 1024 * 1024;
    std::vector<uint8_t> nca(0x4000 + outputChunk + 123, 0);
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNcz(nca)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files[0] == nca);
    assert(std::find(capture.writeSizes.begin(), capture.writeSizes.end(),
                     outputChunk) != capture.writeSizes.end());
}

void testEncryptedNsz() {
    std::vector<uint8_t> nca(0x4000 + 300123);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 41) ^ (i >> 5));
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNcz(nca, true)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files.size() == 1);
    assert(capture.files[0] == nca);
}

void testOfficialLayoutNsz() {
    std::vector<uint8_t> nca(0x4000 + 210123);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 37) ^ (i >> 6));
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNcz(
                                  nca, false,
                                  NczTestLayout::OfficialCountBeforeSections)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files.size() == 1);
    assert(capture.files[0] == nca);
}

void testLegacySectionListNsz() {
    std::vector<uint8_t> nca(0x4000 + 90000);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 19) ^ (i >> 4));
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNcz(
                                  nca, false,
                                  NczTestLayout::LegacySectionList)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files.size() == 1);
    assert(capture.files[0] == nca);
}

void testLooseNczSectionFields() {
    std::vector<uint8_t> nca(0x4000 + 85000);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 11) ^ (i >> 3));
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNcz(
                                  nca, false,
                                  NczTestLayout::LegacyCountAfterMagic,
                                  9, 0x12345678)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files.size() == 1);
    assert(capture.files[0] == nca);
}

void testManyNczSections() {
    constexpr size_t sectionCount = 96;
    constexpr size_t sectionSize = 1024;
    std::vector<uint8_t> nca(0x4000 + sectionCount * sectionSize);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 7) ^ (i >> 2));
    std::vector<std::pair<uint64_t, uint64_t>> sections;
    for (size_t i = 0; i < sectionCount; ++i) {
        sections.emplace_back(0x4000 + i * sectionSize, sectionSize);
    }
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNczWithSections(nca, sections)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files.size() == 1);
    assert(capture.files[0] == nca);
}

void testNczSectionStartsBeforeHeaderEnd() {
    std::vector<uint8_t> nca(0x4000 + 50000);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 31) ^ (i >> 9));
    std::vector<std::pair<uint64_t, uint64_t>> sections {
        {0x3000, 0x3000},
        {0x6000, static_cast<uint64_t>(nca.size() - 0x6000)},
    };
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNczWithSections(nca, sections)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files.size() == 1);
    assert(capture.files[0] == nca);
}

void testBlockNsz() {
    std::vector<uint8_t> nca(0x4000 + 190321);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 17) + (i >> 7));
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeBlockNcz(nca)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files.size() == 1);
    assert(capture.files[0] == nca);
}

void testOfficialBlockNsz() {
    std::vector<uint8_t> nca(0x4000 + 190321);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 23) + (i >> 9));
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeBlockNcz(
                                  nca,
                                  NczTestLayout::OfficialCountBeforeSections)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    feed(stream, package);
    assert(capture.files.size() == 1);
    assert(capture.files[0] == nca);
}

void testSkipFileDropsEntryWithoutCallbacks() {
    std::vector<uint8_t> first {1, 2, 3, 4, 5};
    std::vector<uint8_t> delta(50000, 0xAB);
    std::vector<uint8_t> last(1000);
    for (size_t i = 0; i < last.size(); ++i)
        last[i] = static_cast<uint8_t>(i * 3);
    const std::string deltaName = "0123456789abcdef0123456789abcdef.nca";
    auto package = makePfs0({{"first.nca", first},
                             {deltaName, delta},
                             {"last.nca", last}});
    Capture capture;
    PackageCallbacks callbacks = capture.callbacks();
    std::vector<std::string> queried;
    callbacks.skipFile = [&](const std::string& name) {
        queried.push_back(name);
        return name == deltaName;
    };
    PackageStream stream(false, std::move(callbacks));
    feed(stream, package);
    assert((queried == std::vector<std::string>{
        "first.nca", deltaName, "last.nca"}));
    assert((capture.names == std::vector<std::string>{
        "first.nca", "last.nca"}));
    assert(capture.files[0] == first);
    assert(capture.files[1] == last);
    assert(stream.consumed() == package.size());
}

void testSkippedNczEntryBypassesDecoder() {
    // The skipped entry is deliberately NOT a valid NCZ: if the stream tried
    // to decode it instead of raw-consuming its bytes, the feed would fail.
    std::vector<uint8_t> garbage(0x5000, 0x5A);
    std::vector<uint8_t> nca(0x4000 + 200000);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 29) ^ (i >> 8));
    auto package = makePfs0(
        {{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.ncz", garbage},
         {"00112233445566778899aabbccddeeff.ncz", makeSolidNcz(nca)}});
    Capture capture;
    PackageCallbacks callbacks = capture.callbacks();
    callbacks.skipFile = [](const std::string& name) {
        // The stream must announce the .nca name, matching what the backend
        // parses for content ids.
        return name == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.nca";
    };
    PackageStream stream(true, std::move(callbacks));
    feed(stream, package);
    assert(capture.names.size() == 1);
    assert(capture.names[0] == "00112233445566778899aabbccddeeff.nca");
    assert(capture.files[0] == nca);
}

// Emulates an interrupted install (IMPROVEMENT_PLAN F-B): feed `package`
// up to `cut`, keeping the latest checkpoint plus a snapshot of the capture
// taken at checkpoint time (the journal analogue — installer writes past
// the checkpoint are overwritten on resume). Then restore a second stream
// from the checkpoint and feed the rest starting at state.consumed.
Capture runResume(bool compressed, const std::vector<uint8_t>& package,
                  size_t cut) {
    assert(cut < package.size());
    Capture capture;
    Capture snapshot;
    pipensx::install::PackageStreamState state;
    bool haveState = false;
    {
        PackageStream stream(compressed, capture.callbacks());
        static const size_t chunks[] = {4093, 65537, 31, 7, 129, 65536};
        size_t offset = 0;
        size_t index = 0;
        while (offset < cut) {
            size_t count = std::min(chunks[index++ % 6], cut - offset);
            if (!stream.write(package.data() + offset, count)) {
                std::cerr << stream.error() << '\n';
                assert(false);
            }
            offset += count;
            pipensx::install::PackageStreamState candidate;
            if (stream.checkpoint(candidate)) {
                state = std::move(candidate);
                snapshot = capture;
                haveState = true;
            }
        }
        // Stream is dropped here mid-package: the "crash".
    }
    assert(haveState);
    assert(state.consumed <= cut);

    Capture resumed = snapshot;
    PackageStream stream(compressed, resumed.callbacks());
    assert(stream.restore(state));
    assert(stream.consumed() == state.consumed);
    std::vector<uint8_t> rest(package.begin() + state.consumed, package.end());
    feed(stream, rest);
    assert(stream.consumed() == package.size());
    return resumed;
}

void testResumeNsp() {
    std::vector<uint8_t> first {1, 2, 3, 4, 5};
    std::vector<uint8_t> second(100000);
    for (size_t i = 0; i < second.size(); ++i)
        second[i] = static_cast<uint8_t>(i * 17);
    auto package = makePfs0({{"first.nca", first}, {"second.nca", second}});
    for (size_t cut : {size_t{200}, size_t{5000}, size_t{50017},
                       package.size() - 3}) {
        Capture resumed = runResume(false, package, cut);
        assert((resumed.names == std::vector<std::string>{
            "first.nca", "second.nca"}));
        assert(resumed.files[0] == first);
        assert(resumed.files[1] == second);
    }
}

void testResumeBlockNsz(bool encrypted) {
    std::vector<uint8_t> nca(0x4000 + 190321);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 17) + (i >> 7));
    auto package = makePfs0(
        {{"00112233445566778899aabbccddeeff.ncz",
          makeBlockNcz(nca, NczTestLayout::LegacyCountAfterMagic,
                       encrypted)}});
    // Cuts inside the buffered NCA/NCZ header (decoder restarts fresh),
    // mid-block-stream (block-boundary rollback, AES counter mid-section),
    // and near the end. The compressed body is small, so derive cuts from
    // the actual package size.
    for (size_t cut : {size_t{0x1000},
                       size_t{0x4000} + 64,
                       0x4000 + (package.size() - 0x4000) / 2,
                       package.size() - 5}) {
        Capture resumed = runResume(true, package, cut);
        assert(resumed.names.size() == 1);
        assert(resumed.names[0] == "00112233445566778899aabbccddeeff.nca");
        assert(resumed.files[0] == nca);
    }
}

void testResumeSolidNsz() {
    std::vector<uint8_t> first(30000);
    for (size_t i = 0; i < first.size(); ++i)
        first[i] = static_cast<uint8_t>(i * 7);
    std::vector<uint8_t> nca(0x4000 + 200000);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 29) ^ (i >> 8));
    auto package = makePfs0({{"first.nca", first},
                             {"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNcz(nca, true)}});
    // Cuts mid-solid roll back to the last safe point before the Zstandard
    // stream consumed input; the solid entry replays from re-fed bytes.
    for (size_t cut : {size_t{10000}, size_t{35000},
                       package.size() - 100}) {
        Capture resumed = runResume(true, package, cut);
        assert((resumed.names == std::vector<std::string>{
            "first.nca", "00112233445566778899aabbccddeeff.nca"}));
        assert(resumed.files[0] == first);
        assert(resumed.files[1] == nca);
    }
}

void testSolidMidEntryNotCheckpointable() {
    std::vector<uint8_t> nca(0x4000 + 200000);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>((i * 29) ^ (i >> 8));
    auto package = makePfs0({{"00112233445566778899aabbccddeeff.ncz",
                              makeSolidNcz(nca)}});
    Capture capture;
    PackageStream stream(true, capture.callbacks());
    // Feed enough that the solid Zstandard stream has consumed input but the
    // entry has not finished: no safe point exists there.
    assert(stream.write(package.data(), package.size() / 2));
    pipensx::install::PackageStreamState state;
    assert(!stream.checkpoint(state));
}

void testRestoreRejectsInconsistentState() {
    std::vector<uint8_t> first {1, 2, 3, 4, 5};
    auto package = makePfs0({{"first.nca", first}});
    Capture capture;
    pipensx::install::PackageStreamState state;
    {
        PackageStream stream(false, capture.callbacks());
        assert(stream.write(package.data(), package.size() - 2));
        assert(stream.checkpoint(state));
    }
    {
        // Entry index out of range.
        auto bad = state;
        bad.entryIndex = 7;
        Capture sink;
        PackageStream stream(false, sink.callbacks());
        assert(!stream.restore(bad));
    }
    {
        // Position does not match the open entry.
        auto bad = state;
        bad.dataPosition += 1;
        Capture sink;
        PackageStream stream(false, sink.callbacks());
        assert(!stream.restore(bad));
    }
    {
        // Restore on a used stream.
        Capture sink;
        PackageStream stream(false, sink.callbacks());
        assert(stream.write(package.data(), 4));
        assert(!stream.restore(state));
    }
}

void* runNszOnSmallStack(void*) {
    testNsz();
    return nullptr;
}

void testNszSmallWorkerStack() {
    pthread_attr_t attributes;
    assert(pthread_attr_init(&attributes) == 0);
    assert(pthread_attr_setstacksize(&attributes, 128 * 1024) == 0);
    pthread_t worker;
    assert(pthread_create(&worker, &attributes,
                          runNszOnSmallStack, nullptr) == 0);
    pthread_attr_destroy(&attributes);
    assert(pthread_join(worker, nullptr) == 0);
}

} // namespace

int main() {
    testNsp();
    testNsz();
    testNszUsesFourMiBOutputChunks();
    testEncryptedNsz();
    testOfficialLayoutNsz();
    testLegacySectionListNsz();
    testLooseNczSectionFields();
    testManyNczSections();
    testNczSectionStartsBeforeHeaderEnd();
    testBlockNsz();
    testOfficialBlockNsz();
    testSkipFileDropsEntryWithoutCallbacks();
    testSkippedNczEntryBypassesDecoder();
    testResumeNsp();
    testResumeBlockNsz(false);
    testResumeBlockNsz(true);
    testResumeSolidNsz();
    testSolidMidEntryNotCheckpointable();
    testRestoreRejectsInconsistentState();
    testNszSmallWorkerStack();
    std::cout << "package stream tests passed\n";
    return 0;
}
