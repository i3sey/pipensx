#include "../src/app/port_archive.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pipensx;

namespace {

void writeFile(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool run(const std::string& cmd) {
    return std::system(cmd.c_str()) == 0;
}

} // namespace

int main() {
    assert(portArchiveSolidFitsRam(0, 0));
    assert(!portArchiveSolidFitsRam(100, kPortArchiveSolidRamReserveBytes));

    const fs::path root = "/tmp/pipensx-port-archive";
    fs::remove_all(root);
    fs::create_directories(root / "src/game");
    fs::create_directories(root / "src/other");
    fs::create_directories(root / "out");

    const std::string payload(2 * 1024 * 1024 + 123, 'P');
    const std::string small = "hello-port";
    const std::string nro = "NRO-PAYLOAD";
    writeFile(root / "src/game/game.nro", nro);
    writeFile(root / "src/game/data.bin", payload);
    writeFile(root / "src/game/readme.txt", small);
    writeFile(root / "src/other/skip.bin", std::string(64 * 1024, 'X'));

    const fs::path archive = root / "game-data.7z";
    // Solid LZMA2 so extract takes the streaming path (>1 MiB folder).
    assert(run("cd '" + (root / "src").string() +
               "' && 7z a -t7z -m0=LZMA2 -ms=on '" + archive.string() +
               "' game other >/dev/null"));

    PortArchiveProbe probe;
    assert(probePortArchive(archive.string(), probe));
    assert(probe.ok);
    assert(probe.switchFiles == 3);
    assert(probe.unpackBytes == nro.size() + payload.size() + small.size());
    assert(probe.files[0].rfind("game/", 0) == 0);
    assert(probe.maxSolidBlockBytes >= payload.size());

    std::atomic<bool> cancelled{false};
    std::string error;
    uint64_t progressed = 0;
    assert(extractPortArchive(
        archive.string(), (root / "out").string(), cancelled,
        [&](uint64_t n) { progressed += n; }, nullptr, error));
    assert(error.empty());
    assert(progressed == nro.size() + payload.size() + small.size());
    assert(readFile(root / "out/game/game.nro") == nro);
    assert(readFile(root / "out/game/data.bin") == payload);
    assert(readFile(root / "out/game/readme.txt") == small);
    assert(!fs::exists(root / "out/../other/skip.bin"));
    assert(!fs::exists(root / "out/other/skip.bin"));

    // A second extraction must report a conflict instead of replacing files
    // that may now contain user configuration or saves.
    progressed = 0;
    error.clear();
    assert(!extractPortArchive(
        archive.string(), (root / "out").string(), cancelled,
        [&](uint64_t n) { progressed += n; }, nullptr, error));
    assert(!error.empty());
    assert(readFile(root / "out/game/data.bin") == payload);

    fs::remove_all(root);
    std::cout << "port archive tests passed\n";
    return 0;
}
