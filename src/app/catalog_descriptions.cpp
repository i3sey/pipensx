#include "catalog_descriptions.hpp"
#include "snapshot_zstd.hpp"
extern "C" {
#include "../core/sha1.h"
}
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace pipensx {
namespace {
uint32_t little32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
constexpr size_t kBlockLimit = 1u << 20;
}
std::string descriptionLocale(const std::string& locale) {
    if (locale == "pt" || locale == "pt-BR" || locale == "pt-PT") return "pt-BR";
    if (locale == "en" || locale == "en-US" || locale == "en-GB") return "en-US";
    if (locale == "zh" || locale == "zh-CN" || locale == "zh-Hans") return "zh-CN";
    for (const char* language : {"ru", "fr", "es", "it", "ko"})
        if (locale == language || locale.rfind(std::string(language) + "-", 0) == 0)
            return language;
    return {};
}
OfflineCatalogDescriptions::OfflineCatalogDescriptions(std::string root)
    : root_(std::move(root)) {}
bool OfflineCatalogDescriptions::load(const std::string& locale) {
    locale_ = locale;
    records_.clear();
    block_.clear();
    blockOffset_ = 0;
    File file(std::fopen((root_ + "/" + locale + ".pdx").c_str(), "rb"), std::fclose);
    if (!file) return false;
    uint8_t header[16];
    if (std::fread(header, 1, sizeof(header), file.get()) != sizeof(header) ||
        std::memcmp(header, "PNXDESC1", 8) != 0 || little32(header + 8) != 1)
        return false;
    const uint32_t count = little32(header + 12);
    if (!count || count > 20000 || std::fseek(file.get(), 0, SEEK_END) != 0) return false;
    const long length = std::ftell(file.get());
    const uint64_t indexEnd = 16ull + count * 40ull;
    if (length < 0 || static_cast<uint64_t>(length) < indexEnd ||
        std::fseek(file.get(), 16, SEEK_SET) != 0) return false;
    std::vector<Record> records;
    records.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t bytes[40];
        if (std::fread(bytes, 1, sizeof(bytes), file.get()) != sizeof(bytes)) return false;
        Record record;
        std::copy(bytes, bytes + 20, record.hash.begin());
        record.block = little32(bytes + 20);
        record.compressed = little32(bytes + 24);
        record.decoded = little32(bytes + 28);
        record.begin = little32(bytes + 32);
        record.size = little32(bytes + 36);
        if (record.block < indexEnd || !record.compressed || !record.decoded ||
            record.compressed > kBlockLimit || record.decoded > kBlockLimit ||
            !record.size || record.size > 65536 ||
            uint64_t(record.block) + record.compressed > uint64_t(length) ||
            uint64_t(record.begin) + record.size > record.decoded ||
            (!records.empty() && !(records.back().hash < record.hash))) return false;
        records.push_back(record);
    }
    records_ = std::move(records);
    return true;
}
bool OfflineCatalogDescriptions::lookup(const std::string& source,
                                        const std::string& requestedLocale,
                                        std::string& translated) {
    translated.clear();
    const std::string locale = descriptionLocale(requestedLocale);
    if (source.empty() || locale.empty()) return false;
    std::array<uint8_t, 20> hash{};
    sha1(source.data(), source.size(), hash.data());
    std::lock_guard<std::mutex> lock(mutex_);
    if (locale_ != locale && !load(locale)) return false;
    auto found = std::lower_bound(records_.begin(), records_.end(), hash,
        [](const Record& record, const auto& value) { return record.hash < value; });
    if (found == records_.end() || found->hash != hash) return false;
    if (blockOffset_ != found->block || block_.size() != found->decoded) {
        File file(std::fopen((root_ + "/" + locale + ".pdx").c_str(), "rb"), std::fclose);
        if (!file || std::fseek(file.get(), found->block, SEEK_SET) != 0) return false;
        std::string block(found->compressed, '\0'), error;
        if (std::fread(block.data(), 1, block.size(), file.get()) != block.size() ||
            !decompressZstd(block, kBlockLimit, error) || block.size() != found->decoded)
            return false;
        block_ = std::move(block);
        blockOffset_ = found->block;
    }
    translated = block_.substr(found->begin, found->size);
    return true;
}
bool OfflineCatalogDescriptions::contains(const std::string& source,
                                          const std::string& requestedLocale) {
    const std::string locale = descriptionLocale(requestedLocale);
    if (locale.empty() || source.empty()) return false;
    std::array<uint8_t, 20> hash{};
    sha1(source.data(), source.size(), hash.data());
    std::lock_guard<std::mutex> lock(mutex_);
    if (locale_ != locale && !load(locale)) return false;
    auto found = std::lower_bound(records_.begin(), records_.end(), hash,
        [](const Record& record, const auto& value) { return record.hash < value; });
    return found != records_.end() && found->hash == hash;
}
namespace {
OfflineCatalogDescriptions& bundledDescriptions() {
#ifdef __SWITCH__
    static OfflineCatalogDescriptions descriptions("romfs:/catalog/descriptions");
#else
    static OfflineCatalogDescriptions descriptions("resources/catalog/descriptions");
#endif
    return descriptions;
}
}
bool offlineCatalogDescription(const std::string& source, const std::string& locale,
                               std::string& translated) {
    return bundledDescriptions().lookup(source, locale, translated);
}
bool hasOfflineCatalogDescription(const std::string& source, const std::string& locale) {
    return bundledDescriptions().contains(source, locale);
}
} // namespace pipensx
