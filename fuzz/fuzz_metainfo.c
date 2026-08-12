/* libFuzzer entry for .torrent / magnet info-dict parsing. */
#include "../src/core/metainfo.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data)
        return 0;
    metainfo_t mi;
    if (metainfo_parse(data, size, &mi))
        metainfo_free(&mi);
    return 0;
}
