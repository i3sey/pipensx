#include "app/remote_archive.hpp"
#include "zip_fixture.hpp"
#include <cstdio>

using namespace pipensx;

int main() {
    // Compact UTF-16 RAR4 names and 64-bit payloads use a different layout
    // from ZIP64. Exercise both independently of the ZIP parser.
    for (bool sparse : {false, true}) {
        const std::u16string unicode = u"\u4e2d\u6587.nsp";
        std::string encoded("X.nsp\0", 6);
        encoded.push_back(0);
        for (size_t i = 0; i < unicode.size(); ++i) {
            if (!(i % 4)) encoded.push_back(static_cast<char>(0xaa));
            zip_fixture::number(encoded, unicode[i], 2);
        }
        auto bytes = zip_fixture::rar4({{encoded, "data", false}}, true);
        constexpr size_t fileHeader = 20;
        const size_t headerBytes = static_cast<uint8_t>(bytes[25]) | (static_cast<uint8_t>(bytes[26]) << 8);
        bytes[fileHeader + 4] |= 2; // LHD_UNICODE.
        const uint64_t payloadBytes = sparse ? (uint64_t(5) << 30) + 1234 : 4;
        for (size_t i = 0; i < 4; ++i) {
            bytes[fileHeader + 7 + i] = bytes[fileHeader + 11 + i] = static_cast<char>(payloadBytes >> (i * 8));
            bytes[fileHeader + 32 + i] = bytes[fileHeader + 36 + i] = static_cast<char>(payloadBytes >> (32 + i * 8));
        }
        const auto crc = crc32(0, reinterpret_cast<const Bytef*>(bytes.data() + fileHeader + 2), headerBytes - 2);
        bytes[fileHeader] = static_cast<char>(crc); bytes[fileHeader + 1] = static_cast<char>(crc >> 8);
        const size_t dataOffset = fileHeader + headerBytes;
        const auto suffix = bytes.substr(dataOffset + 4);
        const uint64_t total = dataOffset + payloadBytes + suffix.size();
        RangeFetcher fetch = [&](const std::string&, uint64_t start, uint64_t end,
            const std::function<bool(const uint8_t*,size_t)>& sink,
            const std::function<bool()>&, std::string&) {
            assert(end && end <= total);
            std::string data(end - start, '\0');
            for (size_t i = 0; i < data.size(); ++i) {
                const uint64_t at = start + i;
                if (at < dataOffset) data[i] = bytes[at];
                else if (at >= dataOffset + payloadBytes) data[i] = suffix[at - dataOffset - payloadBytes];
                else if (!sparse) data[i] = bytes[at];
            }
            return sink(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        };
        std::string error;
        RemoteArchive archive("https://example/rar", total, fetch);
        assert(archive.open([] { return false; }, error));
        assert(archive.entries().size() == 1 && archive.entries()[0].path == "中文.nsp");
        assert(archive.entries()[0].bytes == payloadBytes);
        assert(archive.prepare(0, [] { return false; }, error));
        std::string received;
        const uint64_t start = sparse ? (uint64_t(4) << 30) + 456 : 0;
        const uint64_t end = sparse ? start + 128 : 0;
        assert(archive.read(0, start, end,
            [&](const uint8_t* p,size_t n) { received.append(reinterpret_cast<const char*>(p),n); return true; },
            [] { return false; }, error));
        assert(received == (sparse ? std::string(128, '\0') : "data"));
    }
    for (bool large : {false, true}) {
        const std::string content(900000, 'r');
        auto bytes = zip_fixture::rar4({{"folder\\game.nsz", content, false}}, large);
        auto fetch = [&](const std::string&, uint64_t start, uint64_t end,
            const std::function<bool(const uint8_t*,size_t)>& sink,
            const std::function<bool()>& stopped, std::string&) {
            assert(end && end <= bytes.size());
            while (start < end) {
                if (stopped()) return false;
                const size_t n = std::min<uint64_t>(30000, end - start);
                if (!sink(reinterpret_cast<const uint8_t*>(bytes.data() + start), n)) return false;
                start += n;
            }
            return true;
        };
        std::string error;
        RemoteArchive archive("https://example/rar", bytes.size(), fetch);
        assert(archive.open([] { return false; }, error));
        assert(std::string(archive.format()) == "RAR4");
        assert(archive.entries().size() == 1 && archive.entries()[0].path == "folder/game.nsz");
        assert(archive.prepare(0, [] { return false; }, error));
        for (uint64_t start : {0, 600000}) {
            std::string received;
            assert(archive.read(0, start, 0,
                [&](const uint8_t* p,size_t n) { received.append(reinterpret_cast<const char*>(p),n); return true; },
                [] { return false; }, error));
            assert(received == content.substr(start));
        }
        bytes[archive.entries()[0].dataOffset] ^= 1;
        assert(!archive.read(0, 0, 0, [](const uint8_t*,size_t) { return true; }, [] { return false; }, error));
        bytes[9] ^= 1; // Main header CRC detects tampering before payload fetch.
        assert(!archive.open([] { return false; }, error));
    }
    for (bool deflated : {false, true}) for (bool zip64 : {false, true}) {
        const uint64_t prefix = zip64 ? (uint64_t(5) << 30) : 0;
        const std::string payload(900000, 'x');
        const auto bytes = zip_fixture::build({{"folder/中文.nsp", payload, deflated}}, zip64, prefix);
        RangeFetcher fetch = [&](const std::string&, uint64_t start, uint64_t end,
            const std::function<bool(const uint8_t*, size_t)>& sink,
            const std::function<bool()>& stop, std::string&) {
            assert(end && end >= start && end <= prefix + bytes.size());
            // Sparse prefix is not archive payload; only the EOCD tail may
            // read it. This exercises 64-bit offsets without a huge fixture.
            while (start < end) {
                if (stop()) return false;
                const size_t n = std::min<uint64_t>(17001, end - start);
                std::string piece(n, '\0');
                for (size_t i = 0; i < n; ++i)
                    if (start + i >= prefix) piece[i] = bytes[start + i - prefix];
                if (!sink(reinterpret_cast<const uint8_t*>(piece.data()), n)) return false;
                start += n;
            }
            return true;
        };
        std::string error;
        RemoteArchive zip("https://example/archive", prefix + bytes.size(), fetch);
        assert(zip.open([] { return false; }, error));
        assert(zip.entries().size() == 1);
        assert(zip.prepare(0, [] { return false; }, error));
        for (const auto range : {std::pair<uint64_t,uint64_t>{0, 0}, {123456, 0}, {4321, 6543}}) {
            std::string received;
            assert(zip.read(0, range.first, range.second,
                [&](const uint8_t* p, size_t n) { received.append(reinterpret_cast<const char*>(p), n); return true; },
                [] { return false; }, error));
            assert(received == payload.substr(range.first, (range.second ? range.second : payload.size()) - range.first));
        }
        bool stop = false;
        assert(!zip.read(0, 0, 0, [&](const uint8_t*, size_t) { stop = true; return false; },
                         [&] { return stop; }, error));
    }
    const auto fixture = zip_fixture::build({{"game.nsp", "data", false}});
    for (int scenario = 0; scenario < 4; ++scenario) {
        auto bytes = fixture;
        if (scenario == 0) bytes.resize(bytes.size() - 1);
        if (scenario == 1) bytes[30 + std::string("game.nsp").size()] ^= 1;
        if (scenario == 2) { // Matching encrypted local/central headers.
            bytes[6] |= 1;
            bytes[bytes.find(std::string("PK\1\2", 4)) + 8] |= 1;
        }
        RangeFetcher fetch = [&](const std::string&, uint64_t start, uint64_t end,
            const std::function<bool(const uint8_t*,size_t)>& sink,
            const std::function<bool()>&, std::string& error) {
            if (scenario == 3) { error = kDebridRangeNotSupported; return false; }
            assert(end && end <= bytes.size());
            return sink(reinterpret_cast<const uint8_t*>(bytes.data() + start), end - start);
        };
        std::string error;
        RemoteArchive zip("https://example/archive", bytes.size(), fetch);
        const bool ok = zip.open([] { return false; }, error) &&
            zip.prepare(0, [] { return false; }, error) &&
            zip.read(0, 0, 0, [](const uint8_t*,size_t) { return true; }, [] { return false; }, error);
        assert(!ok && !error.empty());
    }
    std::puts("test_remote_archive: all assertions passed");
}
