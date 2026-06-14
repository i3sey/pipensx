#include "install/package_stream.hpp"

#include <zstd.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
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

std::vector<uint8_t> makeSolidNcz(const std::vector<uint8_t>& nca) {
    assert(nca.size() > 0x4000);
    const uint64_t bodySize = nca.size() - 0x4000;
    size_t bound = ZSTD_compressBound(bodySize);
    std::vector<uint8_t> compressed(bound);
    size_t compressedSize = ZSTD_compress(
        compressed.data(), compressed.size(), nca.data() + 0x4000,
        bodySize, 3);
    assert(!ZSTD_isError(compressedSize));
    compressed.resize(compressedSize);

    std::vector<uint8_t> out(nca.begin(), nca.begin() + 0x4000);
    out.insert(out.end(), {'N', 'C', 'Z', 'S', 'E', 'C', 'T', 'N'});
    append64(out, 1);
    append64(out, 0x4000);
    append64(out, bodySize);
    append64(out, 1);
    append64(out, 0);
    out.insert(out.end(), 32, 0);
    out.insert(out.end(), compressed.begin(), compressed.end());
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
        assert(stream.write(data.data() + offset, count));
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

} // namespace

int main() {
    testNsp();
    testNsz();
    std::cout << "package stream tests passed\n";
    return 0;
}
