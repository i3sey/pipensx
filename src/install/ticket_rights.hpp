#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pipensx::install {

inline constexpr size_t kTicketRightsIdOffset = 0x160;
inline constexpr size_t kTicketMinSize = 0x170;

inline bool parseTicketRightsId(const uint8_t* data, size_t size,
                                uint8_t out[16]) {
    if (!data || !out || size < kTicketMinSize)
        return false;
    std::memcpy(out, data + kTicketRightsIdOffset, 16);
    return true;
}

inline bool rightsIdEqual(const uint8_t a[16], const uint8_t b[16]) {
    return std::memcmp(a, b, 16) == 0;
}

} // namespace pipensx::install
