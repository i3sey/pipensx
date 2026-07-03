#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pipensx::install {

class InstallBackend {
public:
    virtual ~InstallBackend() = default;

    virtual bool beginPackage(const std::string& taskId,
                              const std::string& packageName) = 0;
    // Entries the backend wants dropped before any processing (PERF_PLAN 3.4:
    // delta fragments identified by an early CNMT parse). Default: keep all.
    virtual bool shouldSkipFile(const std::string& name) const {
        (void)name;
        return false;
    }
    virtual bool beginFile(const std::string& name, uint64_t size) = 0;
    virtual bool setFileSize(uint64_t size) = 0;
    virtual bool writeFile(const uint8_t* data, size_t size) = 0;
    virtual bool endFile() = 0;
    virtual bool commitPackage(bool& alreadyInstalled) = 0;
    virtual void rollbackPackage() = 0;
    virtual uint64_t installedBytes() const = 0;
    virtual uint64_t expectedBytes() const = 0;
    virtual const std::string& error() const = 0;
};

std::unique_ptr<InstallBackend> createInstallBackend(
    const std::string& workingRoot);

} // namespace pipensx::install
