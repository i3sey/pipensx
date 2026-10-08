#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pipensx {
std::string descriptionLocale(const std::string& locale);
// Only one small index and one bounded compressed-text block reside in RAM.
class OfflineCatalogDescriptions {
public:
    explicit OfflineCatalogDescriptions(std::string root);
    bool lookup(const std::string& source, const std::string& locale,
                std::string& translated);
    bool contains(const std::string& source, const std::string& locale);
private:
    struct Record {
        std::array<uint8_t, 20> hash{};
        uint32_t block = 0, compressed = 0, decoded = 0, begin = 0, size = 0;
    };
    bool load(const std::string& locale);
    std::string root_, locale_;
    std::mutex mutex_;
    std::vector<Record> records_;
    std::string block_;
    uint32_t blockOffset_ = 0;
};
bool offlineCatalogDescription(const std::string& source,
                               const std::string& locale,
                               std::string& translated);
bool hasOfflineCatalogDescription(const std::string& source, const std::string& locale);
} // namespace pipensx
