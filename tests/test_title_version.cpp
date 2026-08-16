#include "app/installed_title_service.hpp"
#include "app/nacp_language.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

namespace {

constexpr size_t kNacpSize = 0x4000;
constexpr size_t kNameSize = 0x200;
constexpr size_t kAuthorSize = 0x100;
constexpr size_t kEntrySize = kNameSize + kAuthorSize;
constexpr size_t kCompressedLangCount = 32;
constexpr size_t kTitlesDataFormatOffset = 0x3215;

std::vector<uint8_t> deflateRaw(const uint8_t* data, size_t len) {
    z_stream stream {};
    assert(deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                        Z_DEFAULT_STRATEGY) == Z_OK);
    std::vector<uint8_t> out(compressBound(static_cast<uLong>(len)));
    stream.next_in = const_cast<Bytef*>(data);
    stream.avail_in = static_cast<uInt>(len);
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    out.resize(static_cast<size_t>(stream.total_out));
    deflateEnd(&stream);
    return out;
}

} // namespace

int main() {
    using pipensx::formatTitleVersion;
    using pipensx::nacpReadLanguage;
    using pipensx::titleVersionIsNewer;

    assert(formatTitleVersion("0") == "0.0.0");
    assert(formatTitleVersion("65536") == "1.0.0");
    assert(formatTitleVersion("131072") == "2.0.0");
    assert(formatTitleVersion("262400") == "4.1.0");
    assert(formatTitleVersion("").empty());
    assert(formatTitleVersion("junk").empty());
    assert(formatTitleVersion("1.0.0").empty());
    assert(formatTitleVersion("-1").empty());

    assert(titleVersionIsNewer("131072", "65536"));
    assert(!titleVersionIsNewer("65536", "131072"));
    assert(!titleVersionIsNewer("65536", "65536"));
    assert(!titleVersionIsNewer("", "65536"));
    assert(!titleVersionIsNewer("131072", "junk"));

    std::vector<uint8_t> plain(kNacpSize, 0);
    std::memcpy(plain.data(), "Breath of the Wild", 18);
    std::memcpy(plain.data() + kNameSize, "Nintendo", 8);
    std::string name;
    std::string author;
    assert(nacpReadLanguage(plain.data(), plain.size(), 0, name, author));
    assert(name == "Breath of the Wild");
    assert(author == "Nintendo");

    std::vector<uint8_t> slots(kCompressedLangCount * kEntrySize, 0);
    const char* zelda = "The Legend of Zelda: Breath of the Wild";
    std::memcpy(slots.data(), zelda, std::strlen(zelda));
    std::memcpy(slots.data() + kNameSize, "Nintendo", 8);
    std::memcpy(slots.data() + 11 * kEntrySize, "Zelda RU", 8);
    const std::vector<uint8_t> compressed =
        deflateRaw(slots.data(), slots.size());
    assert(compressed.size() <= 0x2FFE);

    std::vector<uint8_t> packed(kNacpSize, 0);
    const uint16_t packedSize = static_cast<uint16_t>(compressed.size());
    std::memcpy(packed.data(), &packedSize, sizeof(packedSize));
    std::memcpy(packed.data() + 2, compressed.data(), compressed.size());
    packed[kTitlesDataFormatOffset] = 1;

    assert(nacpReadLanguage(packed.data(), packed.size(), 0, name, author));
    assert(name == zelda);
    assert(author == "Nintendo");
    assert(nacpReadLanguage(packed.data(), packed.size(), 11, name, author));
    assert(name == "Zelda RU");

    packed[kTitlesDataFormatOffset] = 0;
    const bool plainRead =
        nacpReadLanguage(packed.data(), packed.size(), 0, name, author);
    assert(!plainRead || name != zelda);

    std::printf("test_title_version: ok\n");
    return 0;
}
