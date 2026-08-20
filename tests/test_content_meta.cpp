// Tests for the CNMT meta-record patch (issue #60): a title stamped for newer
// firmware must not keep required_system_version in the installed meta
// record, or HOS refuses to launch it with an "update required" nag. Only the
// db record is patched — the NCA payloads stay byte-identical.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "install/content_meta.hpp"

using pipensx::install::kMetaTypeAddOnContent;
using pipensx::install::kMetaTypeApplication;
using pipensx::install::kMetaTypePatch;
using pipensx::install::patchRequiredSystemVersion;

namespace {

// Encoding: (major << 26) | (minor << 20) | (micro << 16).
constexpr uint32_t kRsv21_1_0 = (21u << 26) | (1u << 20) | (0u << 16);

uint32_t u32At(const std::vector<uint8_t>& data, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

} // namespace

int main() {
    // Sanity: 21.1.0 encodes to 0x54100000.
    assert(kRsv21_1_0 == 0x54100000u);

    // Application meta: patch_id (offset 0) and required_application_version
    // (offset 12) untouched, RSV at offset 8 zeroed, original value returned.
    {
        std::vector<uint8_t> header(0x20, 0xAB);
        std::memcpy(header.data() + 8, &kRsv21_1_0, sizeof(kRsv21_1_0));
        const std::vector<uint8_t> original = header;

        const uint32_t rsv = patchRequiredSystemVersion(
            header.data(), static_cast<uint16_t>(header.size()),
            kMetaTypeApplication);
        assert(rsv == kRsv21_1_0);
        assert(u32At(header, 8) == 0);
        assert(std::memcmp(header.data(), original.data(), 8) == 0);
        assert(std::memcmp(header.data() + 12, original.data() + 12,
                           header.size() - 12) == 0);
    }

    // Patch meta: application_id (offset 0) and extended_data_size (offset
    // 12, delta history size) untouched, RSV at the same offset 8 zeroed.
    {
        std::vector<uint8_t> header(0x18, 0xCD);
        const uint32_t extendedDataSize = 0xDEADBEEF;
        std::memcpy(header.data() + 12, &extendedDataSize,
                    sizeof(extendedDataSize));
        std::memcpy(header.data() + 8, &kRsv21_1_0, sizeof(kRsv21_1_0));
        const std::vector<uint8_t> original = header;

        const uint32_t rsv = patchRequiredSystemVersion(
            header.data(), static_cast<uint16_t>(header.size()),
            kMetaTypePatch);
        assert(rsv == kRsv21_1_0);
        assert(u32At(header, 8) == 0);
        assert(u32At(header, 12) == extendedDataSize);
        assert(std::memcmp(header.data(), original.data(), 8) == 0);
    }

    // AddOnContent passes through: offset 8 is required_application_version.
    {
        std::vector<uint8_t> header(0x18, 0xAB);
        const uint32_t appVersion = 0x12345678;
        std::memcpy(header.data() + 8, &appVersion, sizeof(appVersion));
        const std::vector<uint8_t> original = header;

        const uint32_t rsv = patchRequiredSystemVersion(
            header.data(), static_cast<uint16_t>(header.size()),
            kMetaTypeAddOnContent);
        assert(rsv == 0);
        assert(header == original);
    }

    // Other meta types (Delta = 0x83) pass through.
    {
        std::vector<uint8_t> header(0x10, 0xAB);
        const std::vector<uint8_t> original = header;

        const uint32_t rsv = patchRequiredSystemVersion(
            header.data(), static_cast<uint16_t>(header.size()), 0x83);
        assert(rsv == 0);
        assert(header == original);
    }

    // Extended headers shorter than 12 bytes pass through.
    {
        std::vector<uint8_t> header(8, 0xAB);
        const std::vector<uint8_t> original = header;

        const uint32_t rsv = patchRequiredSystemVersion(
            header.data(), static_cast<uint16_t>(header.size()),
            kMetaTypeApplication);
        assert(rsv == 0);
        assert(header == original);
    }

    std::printf("test_content_meta: all ok\n");
    return 0;
}
