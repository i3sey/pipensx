#include "nacp_language.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

namespace pipensx {
namespace {

constexpr size_t kNacpSize = 0x4000;
constexpr size_t kNameSize = 0x200;
constexpr size_t kAuthorSize = 0x100;
constexpr size_t kEntrySize = kNameSize + kAuthorSize;
constexpr size_t kPlainLangCount = 16;
constexpr size_t kCompressedLangCount = 32;
constexpr size_t kTitlesDataFormatOffset = 0x3215;
constexpr size_t kCompressedMax = 0x2FFE;

std::string boundedText(const char* value, size_t size) {
    if (!value || size == 0)
        return {};
    return std::string(value, strnlen(value, size));
}

bool pickLanguage(const uint8_t* entries, size_t count, int preferred,
                  std::string& name, std::string& author) {
    auto tryIndex = [&](size_t index) -> bool {
        const char* n = reinterpret_cast<const char*>(
            entries + index * kEntrySize);
        const char* a = n + kNameSize;
        if (!n[0] && !a[0])
            return false;
        name = boundedText(n, kNameSize);
        author = boundedText(a, kAuthorSize);
        return !name.empty() || !author.empty();
    };
    if (preferred >= 0 && static_cast<size_t>(preferred) < count &&
        tryIndex(static_cast<size_t>(preferred)))
        return true;
    for (size_t i = 0; i < count; ++i) {
        if (tryIndex(i))
            return true;
    }
    return false;
}

bool inflateTitles(const uint8_t* nacp, std::vector<uint8_t>& out) {
    uint16_t packedSize = 0;
    std::memcpy(&packedSize, nacp, sizeof(packedSize));
    if (packedSize == 0 || packedSize > kCompressedMax)
        return false;
    out.assign(kCompressedLangCount * kEntrySize, 0);
    z_stream stream {};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return false;
    stream.next_in = const_cast<Bytef*>(nacp + 2);
    stream.avail_in = packedSize;
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int zrc = inflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    inflateEnd(&stream);
    if (zrc != Z_STREAM_END || produced == 0 || produced % kEntrySize != 0)
        return false;
    out.resize(static_cast<size_t>(produced));
    return true;
}

} // namespace

bool nacpReadLanguage(const void* nacp, size_t size, int preferredIndex,
                      std::string& name, std::string& author) {
    name.clear();
    author.clear();
    if (!nacp || size < kNacpSize)
        return false;
    const auto* bytes = static_cast<const uint8_t*>(nacp);
    const uint8_t format = bytes[kTitlesDataFormatOffset];
    if (format != 0) {
        std::vector<uint8_t> inflated;
        if (!inflateTitles(bytes, inflated))
            return false;
        return pickLanguage(inflated.data(), inflated.size() / kEntrySize,
                            preferredIndex, name, author);
    }
    return pickLanguage(bytes, kPlainLangCount, preferredIndex, name, author);
}

} // namespace pipensx
