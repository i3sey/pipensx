#pragma once

// Sparse NCM placeholder sizing. install_backend_switch.cpp is __SWITCH__-only,
// so the pure size math lives here to stay unit-testable on the PC.
//
// CreatePlaceHolder of a 40+ GiB NCA is a synchronous FAT allocation on the
// same SD the torrent thread logs and checkpoints to (~11s for 43 GiB in
// download-speed-analysis B7). Create a small file so the first writes start
// immediately, then grow with SetPlaceHolderSize in chunks. Do not
// preallocate the whole NCA on a side thread either: that still holds the
// FS lock.

#include <cstdint>

namespace pipensx::install {

constexpr uint64_t kPlaceholderInitialBytes = 64ull * 1024 * 1024;
constexpr uint64_t kPlaceholderGrowChunkBytes = 256ull * 1024 * 1024;

inline uint64_t placeholderCreateBytes(uint64_t size, bool growSupported) {
    if (!growSupported || size <= kPlaceholderInitialBytes)
        return size;
    return kPlaceholderInitialBytes;
}

inline uint64_t placeholderNextBytes(uint64_t allocated, uint64_t needed,
                                     uint64_t size, bool growSupported) {
    if (needed > size)
        needed = size;
    if (allocated >= needed)
        return allocated;
    uint64_t next = size;
    if (growSupported && allocated < size &&
        size - allocated > kPlaceholderGrowChunkBytes) {
        next = allocated + kPlaceholderGrowChunkBytes;
        if (next < needed)
            next = needed;
    }
    return next;
}

} // namespace pipensx::install
