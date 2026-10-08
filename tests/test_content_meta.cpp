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

using pipensx::install::copyFspPath;
using pipensx::install::formatCnmtOpenError;
using pipensx::install::isRomNcaHeaderSignature1VerificationFailed;
using pipensx::install::kFspPathCapacity;
using pipensx::install::kFsRomNcaHeaderSignature1VerificationFailed;
using pipensx::install::kMetaTypeAddOnContent;
using pipensx::install::kMetaTypeApplication;
using pipensx::install::kMetaTypePatch;
using pipensx::install::patchRequiredSystemVersion;
using pipensx::install::resultDescription;
using pipensx::install::resultModule;

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
    // Application-record updates preserve the base and every independent DLC,
    // replacing old versions/storage locations only for the incoming id+type.
    struct MetaKey {
        uint64_t id;
        uint32_t version;
        uint8_t type;
    };
    struct Record {
        MetaKey meta_key;
        uint64_t storage_id;
    };
    using pipensx::install::loadApplicationRecordPages;
    using pipensx::install::replaceApplicationRecordMeta;
    {
        const uint64_t base = 0x0100AAAA00000000ULL;
        std::vector<Record> records{
            {{base, 0, kMetaTypeApplication}, 5},
            {{base + 0x800, 65536, kMetaTypePatch}, 5},
            {{base + 0x1001, 0, kMetaTypeAddOnContent}, 4},
            {{base + 0x1002, 0, kMetaTypeAddOnContent}, 5},
            {{base + 0x800, 131072, kMetaTypePatch}, 4},
        };
        replaceApplicationRecordMeta(records,
            Record{{base + 0x800, 196608, kMetaTypePatch}, 5});
        assert(records.size() == 4);
        assert(records[0].meta_key.id == base);
        assert(records[1].meta_key.id == base + 0x1001);
        assert(records[1].storage_id == 4);
        assert(records[2].meta_key.id == base + 0x1002);
        assert(records[3].meta_key.version == 196608);
        assert(records[3].storage_id == 5);
        replaceApplicationRecordMeta(records,
            Record{{base + 0x1001, 65536, kMetaTypeAddOnContent}, 5});
        assert(records.size() == 4);
        assert(records.back().meta_key.id == base + 0x1001);
        assert(records.back().meta_key.version == 65536);
        // Repeating the same package is idempotent.
        replaceApplicationRecordMeta(records, records.back());
        assert(records.size() == 4);
        assert(records.back().meta_key.id == base + 0x1001);
    }
    {
        std::vector<Record> source;
        for (uint64_t id = 1; id <= 130; ++id)
            source.push_back({{id, 0, kMetaTypeAddOnContent}, 5});
        std::vector<Record> snapshot{{{999, 0, kMetaTypeApplication}, 4}};
        std::vector<size_t> offsets;
        const auto reader = [&](size_t offset, std::vector<Record>& page,
                                size_t& written) {
            offsets.push_back(offset);
            written = std::min(page.size(), source.size() - offset);
            std::copy_n(source.begin() + offset, written, page.begin());
            return true;
        };
        assert(loadApplicationRecordPages(snapshot, reader));
        assert(offsets == std::vector<size_t>({0, 64, 128}));
        assert(snapshot.size() == 130);
        assert(snapshot.back().meta_key.id == 130);
        // Later page failure must not replace the previous full snapshot.
        assert(!loadApplicationRecordPages(snapshot,
            [&](size_t offset, std::vector<Record>& page, size_t& written) {
                return offset == 0 && reader(offset, page, written);
            }));
        assert(snapshot.size() == 130 && snapshot.back().meta_key.id == 130);
        assert(!loadApplicationRecordPages(snapshot,
            [](size_t, std::vector<Record>& page, size_t& written) {
                written = page.size() + 1;
                return true;
            }));
        assert(snapshot.size() == 130);
        // A fresh application legitimately has no record yet.
        assert(loadApplicationRecordPages(snapshot,
            [](size_t, std::vector<Record>&, size_t& written) {
                written = 0;
                return true;
            }));
        assert(snapshot.empty());
    }

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

    // FS Result decode: E5EB bug report used 0xaf5fb402 (module 2, desc 4058).
    {
        constexpr uint32_t kE5ebCnmtOpen = 0xaf5fb402u;
        assert(resultModule(kE5ebCnmtOpen) == 2u);
        assert(resultDescription(kE5ebCnmtOpen) ==
               kFsRomNcaHeaderSignature1VerificationFailed);
        assert(isRomNcaHeaderSignature1VerificationFailed(kE5ebCnmtOpen));
        char text[160];
        const size_t written =
            formatCnmtOpenError(text, sizeof(text), kE5ebCnmtOpen);
        assert(written > 0);
        assert(std::strstr(text, "signature verification failed") != nullptr);
        assert(std::strstr(text, "0xaf5fb402") != nullptr);
    }

    // copyFspPath zero-fills and truncates safely.
    {
        char path[kFspPathCapacity] {};
        path[0] = 'x';
        copyFspPath(path, sizeof(path), "registered:/a/b/c.nca");
        assert(path[0] == 'r');
        assert(std::strcmp(path, "registered:/a/b/c.nca") == 0);
        assert(path[sizeof(path) - 1] == '\0');
        for (size_t i = std::strlen(path) + 1; i < sizeof(path); ++i)
            assert(path[i] == '\0');
    }

    std::printf("test_content_meta: all ok\n");
    return 0;
}
