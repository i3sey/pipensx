// Sparse NCM placeholder sizing (download-speed-analysis B7): a 43 GiB
// CreatePlaceHolder must not land in the first-piece path.
#include <cassert>
#include <cstdio>

#include "install/placeholder_grow.hpp"

using pipensx::install::kPlaceholderGrowChunkBytes;
using pipensx::install::kPlaceholderInitialBytes;
using pipensx::install::placeholderCreateBytes;
using pipensx::install::placeholderNextBytes;

int main() {
    constexpr uint64_t kGta5 = 46354511360ull; // 43.17 GiB NCA from the session

    assert(placeholderCreateBytes(0, true) == 0);
    assert(placeholderCreateBytes(kPlaceholderInitialBytes, true) ==
           kPlaceholderInitialBytes);
    assert(placeholderCreateBytes(kPlaceholderInitialBytes + 1, true) ==
           kPlaceholderInitialBytes);
    assert(placeholderCreateBytes(kGta5, true) == kPlaceholderInitialBytes);
    assert(placeholderCreateBytes(kGta5, false) == kGta5);
    assert(placeholderCreateBytes(4096, false) == 4096);

    // Already covering the write: no grow.
    assert(placeholderNextBytes(kPlaceholderInitialBytes, 4096,
                                kGta5, true) == kPlaceholderInitialBytes);

    // Crossing the initial file grows by one chunk, not the remaining 43 GiB.
    const uint64_t firstGrow = placeholderNextBytes(
        kPlaceholderInitialBytes, kPlaceholderInitialBytes + 1, kGta5, true);
    assert(firstGrow == kPlaceholderInitialBytes + kPlaceholderGrowChunkBytes);
    assert(firstGrow < kGta5);

    // A write that jumps past one chunk still does not snap to the full NCA.
    const uint64_t needed = kPlaceholderInitialBytes +
                            kPlaceholderGrowChunkBytes + 1;
    const uint64_t jumped = placeholderNextBytes(
        kPlaceholderInitialBytes, needed, kGta5, true);
    assert(jumped == needed);
    assert(jumped < kGta5);

    // Last chunk clamps to the NCA size.
    const uint64_t tail = kGta5 - 12345;
    assert(placeholderNextBytes(tail, kGta5, kGta5, true) == kGta5);
    assert(placeholderNextBytes(tail, kGta5 + 99, kGta5, true) == kGta5);

    // Firmware without SetPlaceHolderSize keeps the historical full create.
    assert(placeholderNextBytes(0, 1, kGta5, false) == kGta5);

    // Walk the GTA5 NCA in chunk steps and never emit the full size until
    // the last grow.
    uint64_t allocated = placeholderCreateBytes(kGta5, true);
    uint32_t grows = 0;
    while (allocated < kGta5) {
        const uint64_t next = placeholderNextBytes(
            allocated, allocated + 1, kGta5, true);
        assert(next > allocated);
        assert(next - allocated <= kPlaceholderGrowChunkBytes ||
               next == kGta5);
        allocated = next;
        ++grows;
    }
    assert(allocated == kGta5);
    assert(grows > 1);

    std::printf("test_placeholder_grow: all ok\n");
    return 0;
}
