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
    NczTestLayout layout = NczTestLayout::LegacyCountAfterMagic) {
    constexpr uint8_t exponent = 16;
    constexpr size_t blockSize = size_t{1} << exponent;
    const size_t bodySize = nca.size() - 0x4000;
    std::vector<std::vector<uint8_t>> blocks;
    std::vector<uint32_t> sizes;
    for (size_t offset = 0; offset < bodySize; offset += blockSize) {
        size_t expected = std::min(blockSize, bodySize - offset);
        std::vector<uint8_t> compressed(ZSTD_compressBound(expected));
        size_t result = ZSTD_compress(compressed.data(), compressed.size(),
                                      nca.data() + 0x4000 + offset,
                                      expected, 3);
        assert(!ZSTD_isError(result));
        if (result >= expected) {
            compressed.assign(nca.begin() + 0x4000 + offset,
                              nca.begin() + 0x4000 + offset + expected);
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
    appendNczSection(out, layout, 0x4000, bodySize, 1, empty, empty);
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
    testEncryptedNsz();
    testOfficialLayoutNsz();
    testLegacySectionListNsz();
    testLooseNczSectionFields();
    testManyNczSections();
    testNczSectionStartsBeforeHeaderEnd();
    testBlockNsz();
    testOfficialBlockNsz();
    testNszSmallWorkerStack();
    std::cout << "package stream tests passed\n";
    return 0;
}
