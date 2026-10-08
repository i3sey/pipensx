#pragma once

#include "debrid_transfer.hpp"

namespace pipensx {

// Reads only the ZIP directory and the chosen members through HTTP ranges.
// No archive-sized RAM buffer or temporary copy on the SD card is required.
class RemoteArchive {
public:
    struct Entry {
        std::string path;
        uint64_t bytes = 0, packedBytes = 0, headerOffset = 0, dataOffset = 0;
        uint32_t crc = 0;
        uint16_t method = 0, flags = 0;
        bool prepared = false, symlink = false;
    };
    RemoteArchive(std::string url, uint64_t bytes, RangeFetcher fetcher);
    bool open(const std::function<bool()>& stopped, std::string& error);
    const std::vector<Entry>& entries() const { return entries_; }
    const char* format() const { return rar4_ ? "RAR4" : "ZIP"; }
    // Validate each wanted member before any package is committed.
    bool prepare(size_t index, const std::function<bool()>& stopped,
                 std::string& error);
    bool read(size_t index, uint64_t offset, uint64_t end,
              const std::function<bool(const uint8_t*, size_t)>& sink,
              const std::function<bool()>& stopped, std::string& error) const;
private:
    bool openRar4(const std::function<bool()>& stopped, std::string& error);
    bool readBytes(uint64_t offset, size_t bytes, std::vector<uint8_t>& out,
                   const std::function<bool()>& stopped, std::string& error) const;
    std::string url_;
    uint64_t bytes_, directoryOffset_ = 0;
    RangeFetcher fetcher_;
    std::vector<Entry> entries_;
    bool rar4_ = false;
};

} // namespace pipensx
