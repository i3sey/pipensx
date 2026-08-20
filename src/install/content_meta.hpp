#pragma once

// CNMT meta-record assembly shared by the Switch install backend and the PC
// test suite. install_backend_switch.cpp is __SWITCH__-only, so the pure
// byte-level logic lives here to stay unit-testable on the PC.

#include <cstdint>
#include <cstring>

namespace pipensx::install {

// libnx NcmContentMetaType values as they appear in a packaged CNMT header
// (switch/services/ncm_types.h). Only Application and Patch metas carry
// required_system_version at extended-header offset 8.
enum : uint8_t {
    kMetaTypeApplication = 0x80,
    kMetaTypePatch = 0x81,
    kMetaTypeAddOnContent = 0x82,
};

// Zeroes required_system_version (u32 at offset 8 of the extended header,
// right after the u64 id) in an Application/Patch meta and returns the
// original value. HOS gates title launch on this field as stored in the
// content meta database, so a title stamped for newer firmware refuses to
// launch with an "update required" nag even though its NCAs installed
// byte-identically; Awoo and Tinfoil zero the same field at install. The NCA
// payloads on storage stay untouched — only the db meta record is patched.
// Other meta types and headers shorter than 12 bytes pass through (returns 0).
inline uint32_t patchRequiredSystemVersion(uint8_t* extendedHeader,
                                           uint16_t extendedHeaderSize,
                                           uint8_t metaType) {
    if ((metaType != kMetaTypeApplication && metaType != kMetaTypePatch) ||
        extendedHeaderSize < sizeof(uint64_t) + sizeof(uint32_t)) {
        return 0;
    }
    uint32_t original = 0;
    std::memcpy(&original, extendedHeader + sizeof(uint64_t),
                sizeof(original));
    std::memset(extendedHeader + sizeof(uint64_t), 0, sizeof(original));
    return original;
}

} // namespace pipensx::install
