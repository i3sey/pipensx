#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pipensx::install {

// Where committed content lands. SD is the historical default; NAND
// (BuiltInUser / eMMC) is the write-ceiling experiment from PERF_PLAN 7.4 —
// eMMC is typically faster than the SD/FS path that caps install at ~16 MB/s.
enum class InstallStorageTarget {
    SdCard,
    Nand,
};

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

    // IMPROVEMENT_PLAN F-B: persistent resume of interrupted installs.
    //
    // checkpointPackage() snapshots backend bookkeeping (per-content
    // progress, ncm placeholder ids, the running SHA-256 state) as an opaque
    // blob for the install journal. Must be taken at a package-stream safe
    // point (between writeFile() calls); may flush buffered output so the
    // on-disk state matches the snapshot. Empty = nothing to checkpoint.
    virtual std::string checkpointPackage() { return {}; }

    // Detach from the in-flight package keeping partially written data on
    // disk (files / ncm placeholders) so a later resumePackage() can
    // continue. Backends without resume support fall back to full rollback.
    virtual void suspendPackage() { rollbackPackage(); }

    // Re-attach to a suspended package from a checkpointPackage() blob.
    // Verifies the recorded artifacts still exist and match; on failure any
    // leftover partial data is discarded, the backend stays idle and the
    // caller should fall back to beginPackage(). Streaming then continues
    // with writeFile() from the journaled stream position.
    virtual bool resumePackage(const std::string& taskId,
                               const std::string& packageName,
                               const std::string& state) {
        (void)taskId;
        (void)packageName;
        (void)state;
        return false;
    }

    virtual uint64_t installedBytes() const = 0;
    virtual uint64_t expectedBytes() const = 0;
    virtual const std::string& error() const = 0;
};

std::unique_ptr<InstallBackend> createInstallBackend(
    const std::string& workingRoot,
    InstallStorageTarget target = InstallStorageTarget::SdCard);

} // namespace pipensx::install
