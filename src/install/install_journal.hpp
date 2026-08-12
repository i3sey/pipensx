#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "package_stream.hpp"

namespace pipensx::install {

// Persistent snapshot of an interrupted streaming install
// (IMPROVEMENT_PLAN F-B). Serialized as one bencoded dict: a version tag,
// the package identity (so a stale journal is never applied to a different
// package), the PackageStream safe point and an opaque backend blob with
// backend-specific placeholder bookkeeping.
struct InstallJournal {
    static constexpr int64_t kVersion = 1;

    std::string packageId;     // stable identity (infohash/path/URL)
    uint64_t packageSize = 0;  // total package bytes, 0 when unknown
    bool compressed = false;   // NSZ (true) vs plain NSP
    std::string backendState;  // opaque backend snapshot, may be empty
    PackageStreamState state;

    std::string serialize() const;
    // Strict: any missing key, type mismatch, bad version, malformed
    // length or trailing bytes fails and leaves *this untouched.
    bool load(const char* data, size_t size);
};

// Atomic file helpers (write <path>.tmp, then rename over <path>).
bool saveInstallJournal(const std::string& path, const InstallJournal& journal);
bool loadInstallJournal(const std::string& path, InstallJournal& journal);
// True when the journal no longer exists (including "never existed").
bool removeInstallJournal(const std::string& path);

} // namespace pipensx::install
