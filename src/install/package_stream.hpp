#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace pipensx::install {

struct PackageCallbacks {
    std::function<bool(const std::string&, uint64_t)> beginFile;
    std::function<bool(uint64_t)> setFileSize;
    std::function<bool(const uint8_t*, size_t)> writeFile;
    std::function<bool()> endFile;
};

class PackageStream {
public:
    PackageStream(bool compressed, PackageCallbacks callbacks,
                  std::string telemetryTag = {});
    ~PackageStream();

    PackageStream(const PackageStream&) = delete;
    PackageStream& operator=(const PackageStream&) = delete;

    bool write(const uint8_t* data, size_t size);
    bool finish();
    uint64_t consumed() const;
    const std::string& error() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pipensx::install
