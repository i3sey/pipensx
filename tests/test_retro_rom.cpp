#include "app/retro_rom.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <borealis/extern/nlohmann/json.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <zlib.h>
using namespace pipensx;
namespace fs = std::filesystem;
using Json = nlohmann::json;
static void write(const fs::path &path, const std::string &body) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << body;
}
static std::string read(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::string rom(const std::string &title) {
    std::string body(512, '\0');
    body.replace(0xa0, std::min<size_t>(12, title.size()), title.substr(0, 12));
    body[0xb2] = char(0x96);
    uint8_t sum = 0x19;
    for (size_t i = 0xa0; i <= 0xbc; ++i)
        sum += uint8_t(body[i]);
    body[0xbd] = char(-sum);
    return body;
}
static std::string number(uint64_t value, unsigned bytes) {
    std::string result;
    for (unsigned i = 0; i < bytes; ++i) {
        result += char(value & 255);
        value >>= 8;
    }
    return result;
}
static std::string variable(uint64_t value) {
    std::string result;
    do {
        result += char((value & 127) | (value > 127 ? 128 : 0));
        value >>= 7;
    } while (value);
    return result;
}
static uint32_t crc(const std::string &data) {
    return crc32(0, reinterpret_cast<const Bytef *>(data.data()), data.size());
}
static std::string storedRar(const std::string &name, const std::string &body, bool five,
                             uint32_t mainFlags = 0, uint32_t memberFlags = 0) {
    if (five) {
        const auto block = [](std::string data) {
            data = variable(data.size()) + data;
            return number(crc(data), 4) + data;
        };
        const auto file = variable(2) + variable(2 | memberFlags) + variable(body.size()) +
                          variable(4) + variable(body.size()) + variable(0100644) +
                          number(crc(body), 4) + variable(0) + variable(1) + variable(name.size()) +
                          name;
        return std::string("Rar!\x1a\x07\x01\0", 8) +
               block(variable(1) + variable(0) + variable(mainFlags)) + block(file) + body +
               block(variable(5) + variable(0) + variable(0));
    }
    const auto block = [](const std::string &data) { return number(crc(data), 2) + data; };
    const auto file = number(0x74, 1) + number(0x8000 | memberFlags, 2) +
                      number(32 + name.size(), 2) + number(body.size(), 4) +
                      number(body.size(), 4) + number(3, 1) + number(crc(body), 4) + number(0, 4) +
                      number(20, 1) + number(0x30, 1) + number(name.size(), 2) +
                      number(0100644, 4) + name;
    return std::string("Rar!\x1a\x07\0", 7) +
           block(number(0x73, 1) + number(mainFlags, 2) + number(13, 2) + number(0, 6)) +
           block(file) + body + block(number(0x7b, 1) + number(0, 2) + number(7, 2));
}
static void archiveFile(const fs::path &file,
                        const std::vector<std::pair<std::string, std::string>> &entries,
                        bool seven = false, bool link = false) {
    archive *writer = archive_write_new();
    assert(writer);
    assert((seven ? archive_write_set_format_7zip(writer) : archive_write_set_format_zip(writer)) ==
           ARCHIVE_OK);
    assert(archive_write_open_filename(writer, file.c_str()) == ARCHIVE_OK);
    for (const auto &item : entries) {
        auto *entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.first.c_str());
        archive_entry_set_filetype(entry, link ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        if (link)
            archive_entry_set_symlink(entry, "../outside");
        archive_entry_set_size(entry, link ? 0 : item.second.size());
        assert(archive_write_header(writer, entry) == ARCHIVE_OK);
        if (!link)
            assert(archive_write_data(writer, item.second.data(), item.second.size()) ==
                   ssize_t(item.second.size()));
        archive_entry_free(entry);
    }
    assert(archive_write_close(writer) == ARCHIVE_OK);
    assert(archive_write_free(writer) == ARCHIVE_OK);
}
int main(int argc, char **argv) {
    char temporary[] = "/tmp/pipensx-retro-rom-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path root = temporary;
    fs::create_directories(root / "app");
    fs::create_directories(root / "sd");
    RetroRomStore store((root / "app").string(), (root / "sd").string());
    RetroRelease release;
    release.platform = "gba";
    release.topicId = "123";
    release.title = "Compilation";
    release.infoHash = std::string(40, 'a');
    RetroRomOperation operation;
    std::string error;
    std::atomic<bool> cancelled{false};
    for (const auto *path :
         {"../roms", "/roms", "atmosphere/contents", "switch/pipensx/roms", "roms\\gba"})
        assert(!store.plan(release, path, "mgba", operation, error));
    assert(!store.plan(release, "roms/gba", "unknown", operation, error));
    assert(store.plan(release, "roms/gba", "mgba", operation, error));
    assert(operation.id == "gba-123" && !operation.ready);
    assert(!store.plan(release, "elsewhere", "mgba", operation, error));
    const auto raw = root / "download.gba";
    write(raw, rom("PONG"));
    std::vector<RetroRomInput> files{{"Проверка GBA.gba", raw.string(), 512}};
    cancelled = true;
    assert(!store.prepare(operation.id, files, cancelled, operation, error));
    cancelled = false;
    assert(store.prepare("gba-123", files, cancelled, operation, error));
    assert(operation.ready && operation.games.size() == 1 && operation.games[0].selected &&
           operation.games[0].title == "PONG");
    const auto final = root / "sd/roms/gba/gba-123";
    write(final / "files/game.sav", "preserve save");
    RetroRomOperation repeated;
    assert(store.prepare("gba-123", {}, cancelled, repeated, error));
    assert(repeated.games[0].id == operation.games[0].id &&
           read(final / "files/game.sav") == "preserve save");
    assert(!store.select("gba-123", {"unknown"}, error));
    assert(store.select("gba-123", {}, error));
    assert(store.load("gba-123", repeated, error) && !repeated.games[0].selected);
    assert(store.select("gba-123", {operation.games[0].id}, error));
    write(root / "app/retro/operations/gba-123.json", "corrupt");
    assert(store.load("gba-123", repeated, error));
    assert(store.prepare("gba-123", {}, cancelled, repeated, error) && repeated.games[0].selected);
    std::vector<RetroRomOperation> library;
    assert(store.library(library, error) && library.empty());
    assert(store.addSelectedToLibrary("gba-123", error));
    assert(store.addSelectedToLibrary("gba-123", error));
    RetroRomStore reopened((root / "app").string(), (root / "sd").string());
    assert(reopened.library(library, error) && library.size() == 1 && library[0].games.size() == 1);
    const auto libraryId = repeated.games[0].id;
    std::string resolved;
    assert(reopened.resolveLibraryGame(libraryId, resolved, error));
    assert(resolved == (final / repeated.games[0].path).string());
    assert(!reopened.resolveLibraryGame("gba-123:unknown", resolved, error) && resolved.empty());
    const auto preparedRom = final / repeated.games[0].path;
    fs::rename(preparedRom, final / "hidden.gba");
    assert(!reopened.resolveLibraryGame(libraryId, resolved, error));
    assert(!reopened.addSelectedToLibrary("gba-123", error));
    fs::create_symlink(final / "hidden.gba", preparedRom);
    assert(!reopened.resolveLibraryGame(libraryId, resolved, error));
    fs::remove(preparedRom);
    fs::rename(final / "hidden.gba", preparedRom);
    assert(reopened.resolveLibraryGame(libraryId, resolved, error));
    assert(reopened.removeFromLibrary(libraryId, error));
    assert(reopened.library(library, error) && library.empty());
    assert(fs::exists(preparedRom) && read(final / "files/game.sav") == "preserve save");
    assert(!reopened.resolveLibraryGame(libraryId, resolved, error));
    assert(!reopened.removeFromLibrary(libraryId, error));
    assert(store.select("gba-123", {}, error));
    assert(!store.addSelectedToLibrary("gba-123", error));
    assert(store.select("gba-123", {libraryId}, error));
    assert(store.addSelectedToLibrary("gba-123", error));
    const auto savedOperation = read(root / "app/retro/operations/gba-123.json");
    fs::remove(root / "app/retro/operations/gba-123.json");
    fs::remove(root / "app/retro/operations/gba-123.json.previous");
    assert(reopened.resolveLibraryGame(libraryId, resolved, error));
    write(root / "app/retro/library/gba-123.json", "corrupt");
    assert(reopened.library(library, error));
    // Recovery uses the last valid saved membership (the empty removal record).
    assert(library.empty());
    assert(read(final / "files/game.sav") == "preserve save");
    write(root / "app/retro/operations/gba-123.json", savedOperation);
    auto nextPlan = [&](int topic) {
        release.topicId = std::to_string(topic);
        assert(store.plan(release, "roms/gba", "", operation, error));
    };
    auto input = [&](const fs::path &file) {
        return RetroRomInput{file.filename().string(), file.string(), fs::file_size(file)};
    };
    const auto zip = root / "set.zip";
    archiveFile(zip, {{"one.gba", rom("ONE")}, {"two.gba", rom("TWO")}, {"readme.txt", "readme"}});
    const auto seven = root / "set.7z";
    archiveFile(seven, {{"nested.zip", read(zip)}}, true);
    nextPlan(124);
    assert(store.prepare(operation.id, {input(seven)}, cancelled, operation, error));
    assert(operation.games.size() == 2 && !operation.games[0].selected &&
           operation.games[0].title != operation.games[1].title);
    const auto keep = operation.games[0].id;
    assert(store.select(operation.id, {keep}, error));
    assert(store.load(operation.id, repeated, error) && repeated.games[0].selected &&
           !repeated.games[1].selected);
    assert(store.addSelectedToLibrary(operation.id, error));
    assert(store.library(library, error) && library.size() == 1 && library[0].games.size() == 1);
    assert(store.select(operation.id, {operation.games[1].id}, error));
    assert(store.addSelectedToLibrary(operation.id, error));
    assert(store.library(library, error) && library[0].games.size() == 2);
    assert(store.removeFromLibrary(keep, error));
    assert(store.library(library, error) && library[0].games.size() == 1);
    for (const auto *fixture : {"rar4.rar", "rar5.rar"}) {
        nextPlan(fixture[3] == '4' ? 125 : 126);
        const auto rar = fs::path("tests/fixtures/retro") / fixture;
        if (!store.prepare(operation.id, {input(rar), input(raw)}, cancelled, operation, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        assert(operation.games.size() == 1);
        assert(fs::exists(root / "sd/roms/gba" / operation.id / "unpacked"));
    }
    nextPlan(127);
    const auto bad = root / "bad.zip";
    archiveFile(bad, {{"../escape.gba", rom("BAD")}});
    assert(!store.prepare(operation.id, {input(bad)}, cancelled, operation, error));
    assert(!fs::exists(root / "sd/roms/gba/gba-127") && fs::exists(bad));
    archiveFile(bad, {{"A.gba", rom("A")}, {"a.gba", rom("B")}});
    assert(!store.prepare(operation.id, {input(bad)}, cancelled, operation, error));
    archiveFile(bad, {{"link.gba", ""}}, false, true);
    assert(!store.prepare(operation.id, {input(bad)}, cancelled, operation, error));
    write(bad, "corrupt");
    assert(!store.prepare(operation.id, {input(bad)}, cancelled, operation, error));
    archiveFile(bad, {{"readme.txt", "no game"}});
    assert(!store.prepare(operation.id, {input(bad)}, cancelled, operation, error));
    assert(error.find("No valid GBA") != std::string::npos);
    archiveFile(bad, {{"bad.gba", "not a GBA ROM"}});
    assert(!store.prepare(operation.id, {input(bad)}, cancelled, operation, error));
    const auto deep = root / "deep.zip";
    archiveFile(deep, {{"nested.7z", read(seven)}});
    archiveFile(bad, {{"nested.zip", read(deep)}});
    assert(!store.prepare(operation.id, {input(bad)}, cancelled, operation, error));
    const auto symlink = root / "link.gba";
    fs::create_symlink(raw, symlink);
    assert(!store.prepare(operation.id, {{"link.gba", symlink.string(), 512}}, cancelled, operation,
                          error));
    const auto parentAlias = root / "parent:";
    fs::create_directory_symlink(root, parentAlias);
    assert(!store.prepare(operation.id,
                          {{"safe.gba", (parentAlias / "download.gba").string(), 512}}, cancelled,
                          operation, error));
    write(root / "sd/roms/gba/.pipensx-gba-127.pending/user.txt", "keep user data");
    assert(!store.prepare(operation.id, {input(raw)}, cancelled, operation, error));
    assert(read(root / "sd/roms/gba/.pipensx-gba-127.pending/user.txt") == "keep user data");
    fs::remove_all(root / "sd/roms/gba/.pipensx-gba-127.pending");
    write(root / "sd/roms/gba/gba-127/user.txt", "keep user folder");
    assert(!store.prepare(operation.id, {input(raw)}, cancelled, operation, error));
    assert(read(root / "sd/roms/gba/gba-127/user.txt") == "keep user folder");
    std::vector<RetroRomOperation> operations;
    assert(store.list(operations, error) && operations.size() == 5);
    nextPlan(128);
    const auto stale = root / "sd/roms/gba/.pipensx-gba-128.pending";
    write(stale / "owner.json", Json({{"id", operation.id}, {"task", operation.taskId}}).dump());
    write(stale / "partial.gba", "interrupted extraction");
    assert(store.prepare(operation.id, {input(raw)}, cancelled, operation, error));
    assert(!fs::exists(stale));
    assert(!fs::exists(root / "sd/roms/gba/gba-128/partial.gba"));
    write(root / "app/retro/operations/gba-128.json", Json({{"format", 1},
                                                            {"id", "gba-128"},
                                                            {"task", operation.taskId},
                                                            {"topic", "128"},
                                                            {"title", release.title},
                                                            {"destination", "roms/gba"},
                                                            {"emulator", ""},
                                                            {"ready", false},
                                                            {"games", Json::array()}})
                                                          .dump());
    assert(store.prepare(operation.id, {}, cancelled, repeated, error) && repeated.ready);
    nextPlan(129);
    const auto oversized = root / "oversized.gba";
    write(oversized, "");
    fs::resize_file(oversized, 2ull * 1024 * 1024 * 1024 + 1);
    assert(!store.prepare(operation.id, {input(oversized)}, cancelled, operation, error));
    assert(!fs::exists(root / "sd/roms/gba/gba-129"));
    const auto large = root / "cancel.bin";
    write(large, "");
    fs::resize_file(large, 128 * 1024 * 1024);
    std::atomic<bool> finished{false};
    std::thread cancellation([&] {
        while (!finished &&
               !fs::exists(root / "sd/roms/gba/.pipensx-gba-129.pending/files/cancel.bin"))
            std::this_thread::yield();
        if (!finished)
            cancelled = true;
    });
    const bool completed =
        store.prepare(operation.id, {input(large), input(raw)}, cancelled, operation, error);
    finished = true;
    cancellation.join();
    assert(!completed && cancelled && error == "Cancelled.");
    assert(!fs::exists(root / "sd/roms/gba/.pipensx-gba-129.pending"));
    assert(fs::exists(large) && !fs::exists(root / "sd/roms/gba/gba-129"));
    cancelled = false;
    nextPlan(131);
    const auto unnamed = root / "unnamed.gba";
    write(unnamed, rom(""));
    assert(store.prepare(operation.id, {{".gba", unnamed.string(), 512}}, cancelled, operation,
                         error));
    assert(operation.games[0].title == ".gba");
    for (bool five : {false, true}) {
        nextPlan(five ? 133 : 132);
        const auto archive = root / "stored-game.rar";
        write(archive, storedRar("game.gba", rom("RAR GAME"), five));
        if (!store.prepare(operation.id, {input(archive)}, cancelled, operation, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        assert(operation.games.size() == 1 && operation.games[0].title == "RAR GAME");
        assert(operation.games[0].path.rfind("unpacked/", 0) == 0);
    }
    int compressedTopic = 134;
    for (const auto *fixture : {"rar4-compressed.rar", "rar5-compressed.rar"}) {
        nextPlan(compressedTopic++);
        const auto archive = fs::path("tests/fixtures/retro") / fixture;
        if (!store.prepare(operation.id, {input(archive), input(raw)}, cancelled, operation,
                           error)) {
            std::cerr << error << '\n';
            return 1;
        }
        assert(operation.games.size() == 1);
    }
    nextPlan(136);
    const auto crcArchive = root / "crc-error.rar";
    const auto crcBody = rom("CRC CHECK");
    auto crcData = storedRar("game.gba", crcBody, true);
    const auto bodyStart = crcData.find(crcBody);
    assert(bodyStart != std::string::npos);
    crcData[bodyStart + 300] ^= 1;
    write(crcArchive, crcData);
    assert(!store.prepare(operation.id, {input(crcArchive)}, cancelled, operation, error));
    assert(error.find("CRC") != std::string::npos);
    assert(fs::exists(crcArchive) && !fs::exists(root / "sd/roms/gba/gba-136"));
    for (bool five : {false, true}) {
        write(crcArchive, storedRar("game.gba", crcBody, five, 1));
        assert(!store.prepare(operation.id, {input(crcArchive)}, cancelled, operation, error));
        assert(error.find("volume") != std::string::npos);
    }
    write(crcArchive, storedRar("game.gba", crcBody, false, 0, 4));
    assert(!store.prepare(operation.id, {input(crcArchive)}, cancelled, operation, error));
    assert(error.find("Encrypted") != std::string::npos);
    if (argc > 1) {
        nextPlan(130);
        const auto pong = fs::path(argv[1]);
        assert(store.prepare(operation.id, {input(pong)}, cancelled, operation, error));
        std::cout << "Real homebrew ROM: OK\n";
    }
    if (argc > 2) {
        nextPlan(137);
        const auto rar = fs::path(argv[2]);
        if (!store.prepare(operation.id, {input(rar), input(raw)}, cancelled, operation, error)) {
            std::cerr << "RAR compatibility probe: " << error << '\n';
            fs::remove_all(root);
            return 2;
        }
    }

    // Multi-console tests: direct archive extraction (no duplicate archive copy), PS1 .ccd->.cue & track suppression & .m3u, Vita .vpk untouched, import & settings
    {
        RetroRelease ps1;
        ps1.platform = "ps1";
        ps1.topicId = "5001";
        ps1.title = "Castlevania";
        ps1.infoHash = std::string(40, 'c');
        ps1.coverUrl = "https://example.com/ps1.jpg";
        RetroRomOperation ps1Op;
        assert(store.plan(ps1, "roms/ps1", "duckstation", ps1Op, error));
        assert(ps1Op.id == "ps1-5001" && ps1Op.platform == "ps1");
        RetroRomOperation foundByTask;
        assert(store.findOperationByTask(ps1.infoHash, foundByTask, error) && foundByTask.id == "ps1-5001");

        const auto ps1Zip = root / "castlevania.zip";
        archiveFile(ps1Zip, {
            {"Castlevania.ccd", "[CloneCD]\nVersion=3\n"},
            {"Castlevania.img", std::string(2048, 'D')},
            {"Track 02.bin", std::string(2048, 'A')},
        });
        assert(store.prepare(ps1Op.id, {input(ps1Zip)}, cancelled, ps1Op, error));
        assert(!fs::exists(root / "sd/roms/ps1/ps1-5001/files/castlevania.zip")); // Direct unpack!
        assert(ps1Op.games.size() == 1 &&
               ps1Op.games[0].path.rfind(".cue") == ps1Op.games[0].path.size() - 4);
        assert(store.addSelectedToLibrary(ps1Op.id, error));
        std::string ps1Resolved;
        assert(store.resolveLibraryGame(ps1Op.games[0].id, ps1Resolved, error));
        assert(store.setGameEmulator(ps1Op.games[0].id, "ra_pcsx_rearmed", error));
        RetroRomGame updatedGame;
        assert(store.resolveLibraryGame(ps1Op.games[0].id, ps1Resolved, error, &updatedGame) &&
               updatedGame.emulator == "ra_pcsx_rearmed");
        write(root / "sd/roms/ps1/ps1-5001/unpacked/slot1.mcr", "memcard");
        assert(store.removeFromLibrary(ps1Op.games[0].id, true, error));
        assert(!fs::exists(ps1Resolved));
        assert(read(root / "sd/roms/ps1/ps1-5001/unpacked/slot1.mcr") == "memcard");

        // Multi-disc PS1 -> auto .m3u playlist
        RetroRelease multiDisc = ps1;
        multiDisc.topicId = "5002";
        multiDisc.title = "Final Fantasy VII";
        multiDisc.infoHash = std::string(40, 'd');
        RetroRomOperation mdOp;
        assert(store.plan(multiDisc, "roms/ps1", "", mdOp, error));
        const auto d1 = root / "ff7-d1.chd", d2 = root / "ff7-d2.chd";
        write(d1, std::string(1024, '1'));
        write(d2, std::string(1024, '2'));
        assert(store.prepare(mdOp.id, {
            {"Final Fantasy VII (Disc 1).chd", d1.string(), 1024},
            {"Final Fantasy VII (Disc 2).chd", d2.string(), 1024}
        }, cancelled, mdOp, error));
        assert(mdOp.games.size() == 1 && mdOp.games[0].path.find(".m3u") != std::string::npos);

        // PS Vita .vpk must NOT be unzipped
        RetroRelease vita;
        vita.platform = "psvita";
        vita.topicId = "6001";
        vita.title = "Homebrew Vita";
        vita.infoHash = std::string(40, 'e');
        RetroRomOperation vitaOp;
        assert(store.plan(vita, "roms/psvita", "vita3k", vitaOp, error));
        const auto vpk = root / "game.vpk";
        archiveFile(vpk, {{"eboot.bin", "ELF_FAKE"}, {"sce_sys/param.sfo", "SFO"}});
        assert(store.prepare(vitaOp.id, {{"Homebrew.vpk", vpk.string(), fs::file_size(vpk)}},
                             cancelled, vitaOp, error));
        assert(vitaOp.games.size() == 1 && vitaOp.games[0].path == "files/Homebrew.vpk");

        // PS Vita NoNpDrm unpacked folder dump (eboot.bin + sce_sys/param.sfo)
        RetroRelease vitaNonpdrm;
        vitaNonpdrm.platform = "psvita";
        vitaNonpdrm.topicId = "6002";
        vitaNonpdrm.title = "Uncharted Golden Abyss [NoNpDrm]";
        vitaNonpdrm.infoHash = std::string(40, 'f');
        RetroRomOperation vitaNonpdrmOp;
        assert(store.plan(vitaNonpdrm, "roms/psvita", "vita3k", vitaNonpdrmOp, error));
        const auto npRoot = root / "nonpdrm_src";
        write(npRoot / "app/PCSF00001/eboot.bin", std::string(128, 'E'));
        write(npRoot / "app/PCSF00001/sce_sys/param.sfo", std::string(64, 'S'));
        write(npRoot / "app/PCSF00001/data.bin", std::string(256, 'D'));
        assert(store.prepare(
            vitaNonpdrmOp.id,
            {
                {"app/PCSF00001/eboot.bin", (npRoot / "app/PCSF00001/eboot.bin").string(), 128},
                {"app/PCSF00001/sce_sys/param.sfo", (npRoot / "app/PCSF00001/sce_sys/param.sfo").string(), 64},
                {"app/PCSF00001/data.bin", (npRoot / "app/PCSF00001/data.bin").string(), 256},
            },
            cancelled, vitaNonpdrmOp, error));
        assert(vitaNonpdrmOp.games.size() == 1 &&
               vitaNonpdrmOp.games[0].path == "files/app/PCSF00001/eboot.bin" &&
               vitaNonpdrmOp.games[0].title == "PCSF00001" &&
               vitaNonpdrmOp.games[0].size == 128 + 64 + 256);
        assert(store.addSelectedToLibrary(vitaNonpdrmOp.id, error));
        std::string resolvedVitaPath;
        assert(store.resolveLibraryGame(vitaNonpdrmOp.games[0].id, resolvedVitaPath, error));
        assert(resolvedVitaPath.find("app/PCSF00001/eboot.bin") != std::string::npos);

        // Import directory (and removeFromLibrary must NEVER delete imported user files!)
        const auto userSnes = root / "sd/my_snes";
        write(userSnes / "Super Metroid.sfc", std::string(4096, 'M'));
        std::vector<RetroRomGame> imported;
        assert(store.importDirectory("snes", "sdmc:/my_snes", imported, error) && imported.size() == 1);
        std::string snesPath;
        assert(store.resolveLibraryGame(imported[0].id, snesPath, error));
        assert(snesPath == (userSnes / "Super Metroid.sfc").string());
        assert(store.removeFromLibrary(imported[0].id, true, error));
        assert(fs::exists(userSnes / "Super Metroid.sfc")); // User file preserved!

        // Settings persistence
        RetroSettings settings;
        settings.lastConsole = "ps2";
        settings.defaultEmulator["ps2"] = "nethersx2";
        settings.romDirs["ps2"] = "roms/ps2";
        assert(store.saveSettings(settings, error));
        RetroSettings loadedSettings;
        assert(store.loadSettings(loadedSettings, error) &&
               loadedSettings.lastConsole == "ps2" &&
               loadedSettings.emulatorFor("ps2") == "nethersx2" &&
               loadedSettings.romDirFor("ps2") == "roms/ps2");

        {
            const auto sd = root / "wizard-sd";
            RetroRomStore setupStore((sd / "switch/pipensx").string(), sd.string());
            RetroSettings setup;
            setup.defaultEmulator["gba"] = "ra_mgba";
            setup.nroPaths["gba"] = "sdmc:/switch/custom core.nro";
            setup.biosDirs["gba"] = "firmware";
            setup.saveDirs["gba"] = "my saves/gba";
            setup.stateDirs["gba"] = "my states/gba";
            setup.romDirs["gba"] = "my roms/gba";
            fs::create_directories(sd / "firmware");
            assert(setupStore.saveSettings(setup, error));
            RetroSettings persisted;
            assert(setupStore.loadSettings(persisted, error));
            assert(persisted.biosDirs == setup.biosDirs && persisted.stateDirs == setup.stateDirs);
            assert(persisted.romDirFor("gba") == "my roms/gba");
            assert(persisted.detectionFor("gba", "mgba") == "mgba");
            const auto nro = persisted.detectionFor("gba", "ra_mgba");
            assert(nro == setup.nroPaths.at("gba"));
            std::string arguments;
            assert(setupStore.prepareLaunchArguments(persisted, "gba", "ra_mgba", nro,
                "sdmc:/my roms/gba/game.gba", arguments, error));
            assert(arguments.find("--appendconfig") != std::string::npos);
            assert(fs::is_directory(sd / setup.saveDirs.at("gba")));
            assert(fs::is_directory(sd / setup.stateDirs.at("gba")));
            assert(!RetroRomStore::acceptsSetupDirectory("../escape"));
            assert(setupStore.prepareLaunchArguments(persisted, "gba", "mgba",
                "sdmc:/switch/mgba.nro", "sdmc:/my roms/gba/game.gba", arguments, error));
            assert(arguments.find("--appendconfig") == std::string::npos);
        }

        // .cue FILE case mismatch repair on disc preparation
        RetroRelease cueRel = ps1;
        cueRel.topicId = "5003";
        cueRel.title = "Cue Case";
        cueRel.infoHash = std::string(40, 'f');
        RetroRomOperation cueOp;
        assert(store.plan(cueRel, "roms/ps1", "duckstation", cueOp, error));
        const auto cueZip = root / "cuecase.zip";
        archiveFile(cueZip, {
            {"Castlevania.cue", "FILE \"castlevania.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"},
            {"Castlevania.bin", std::string(2048, 'B')},
        });
        assert(store.prepare(cueOp.id, {input(cueZip)}, cancelled, cueOp, error));
        assert(cueOp.games.size() == 1);
        const auto cueText = read(root / "sd/roms/ps1/ps1-5003" / cueOp.games[0].path);
        assert(cueText.find("\"Castlevania.bin\"") != std::string::npos);

        // Removing all library entries with deleteFiles keeps saves, marks op !ready, and allows re-prepare with save migration
        RetroRelease saveRel;
        saveRel.platform = "gba";
        saveRel.topicId = "7001";
        saveRel.title = "Save Persist";
        saveRel.infoHash = std::string(40, '1');
        RetroRomOperation saveOp;
        assert(store.plan(saveRel, "roms/gba", "mgba", saveOp, error));
        assert(store.prepare(saveOp.id, {input(raw)}, cancelled, saveOp, error));
        assert(store.addSelectedToLibrary(saveOp.id, error));
        const auto saveFile = root / "sd/roms/gba/gba-7001/files/download.sav";
        write(saveFile, "SRAM_DATA_V1");
        assert(store.removeFromLibrary(saveOp.games[0].id, true, error));
        assert(read(saveFile) == "SRAM_DATA_V1");
        RetroRomOperation afterDeleteOp;
        assert(store.load(saveOp.id, afterDeleteOp, error) && !afterDeleteOp.ready && afterDeleteOp.games.empty());
        assert(store.prepare(saveOp.id, {input(raw)}, cancelled, saveOp, error));
        assert(saveOp.ready && saveOp.games.size() == 1 && read(saveFile) == "SRAM_DATA_V1");

        // GBA archive with a mix of valid ROM and junk .gba succeeds with the valid ROM
        nextPlan(7002);
        const auto mixedGba = root / "mixed-gba.zip";
        archiveFile(mixedGba, {
            {"good.gba", rom("VALIDGBA")},
            {"junk.gba", "not a valid gba header"},
        });
        assert(store.prepare(operation.id, {input(mixedGba)}, cancelled, operation, error));
        assert(operation.games.size() == 1 && operation.games[0].title == "VALIDGBA");

        // normalizeDestination handles leading slashes, uppercase SDMC:, duplicate slashes, and swkbd spaces
        assert(RetroRomStore::normalizeDestination("  SDMC:/roms//gba/ ") == "roms/gba");
        assert(RetroRomStore::normalizeDestination("/roms/ps1/") == "roms/ps1");
        assert(RetroRomStore::acceptsDestination("roms/gba"));
        assert(!RetroRomStore::acceptsDestination(RetroRomStore::normalizeDestination("sdmc:/../bad")));

        // Importing a multi-disc folder that already ships an .m3u keeps only the .m3u entry
        const auto mgsDir = root / "sd/my_ps1";
        write(mgsDir / "Metal Gear Solid (Disc 1).chd", std::string(1500, '1'));
        write(mgsDir / "Metal Gear Solid (Disc 2).chd", std::string(2500, '2'));
        write(mgsDir / "Metal Gear Solid.m3u",
              "Metal Gear Solid (Disc 1).chd\r\nMetal Gear Solid (Disc 2).chd\r\n");
        std::vector<RetroRomGame> mgsImported;
        assert(store.importDirectory("ps1", " /my_ps1 ", mgsImported, error));
        assert(mgsImported.size() == 1 && mgsImported[0].path == "Metal Gear Solid.m3u" &&
               mgsImported[0].size == 4000);

        // Preparing an archive that already ships an .m3u keeps only the .m3u entry
        RetroRelease m3uArchiveRel = ps1;
        m3uArchiveRel.topicId = "5004";
        m3uArchiveRel.title = "Xenogears";
        m3uArchiveRel.infoHash = std::string(40, '9');
        RetroRomOperation m3uOp;
        assert(store.plan(m3uArchiveRel, "roms/ps1", "duckstation", m3uOp, error));
        const auto m3uZip = root / "xenogears.zip";
        archiveFile(m3uZip, {
            {"Xenogears (Disc 1).iso", std::string(1200, '1')},
            {"Xenogears (Disc 2).iso", std::string(1800, '2')},
            {"Xenogears.m3u", "Xenogears (Disc 1).iso\nXenogears (Disc 2).iso\n"},
        });
        assert(store.prepare(m3uOp.id, {input(m3uZip)}, cancelled, m3uOp, error));
        assert(m3uOp.games.size() == 1 &&
               m3uOp.games[0].path.rfind("Xenogears.m3u") != std::string::npos &&
               m3uOp.games[0].size == 3000);

        // Settings persist autoCreateForwarder (default true)
        RetroSettings fwdSettings;
        assert(store.loadSettings(fwdSettings, error));
        assert(fwdSettings.autoCreateForwarder == true);
        fwdSettings.autoCreateForwarder = false;
        assert(store.saveSettings(fwdSettings, error));
        RetroSettings fwdReloaded;
        assert(store.loadSettings(fwdReloaded, error));
        assert(fwdReloaded.autoCreateForwarder == false);

        // Operation persists createForwarder through plan() and prepare()
        RetroRelease fwdRel = release;
        fwdRel.topicId = "7010";
        fwdRel.title = "Forwarder Game";
        fwdRel.infoHash = std::string(40, '7');
        RetroRomOperation fwdOp;
        assert(store.plan(fwdRel, "roms/gba", "mgba", fwdOp, error, true));
        assert(fwdOp.createForwarder == true);
        RetroRomOperation fwdLoaded;
        assert(store.load(fwdOp.id, fwdLoaded, error) && fwdLoaded.createForwarder == true);
        assert(store.prepare(fwdOp.id, {input(raw)}, cancelled, fwdOp, error));
        assert(fwdOp.createForwarder == true);

        // planRetroAutoSelection recognizes single-game layouts and skips junk
        std::vector<uint8_t> actions;
        assert(planRetroAutoSelection("gba", false, {"game.gba"}, actions));
        assert((actions == std::vector<uint8_t>{1}));
        assert(planRetroAutoSelection("gba", false, {"game.7z"}, actions));
        assert((actions == std::vector<uint8_t>{1}));
        assert(planRetroAutoSelection("gba", false, {"game.gba", "readme.txt", "cover.jpg"}, actions));
        assert((actions == std::vector<uint8_t>{1, 0, 0}));
        // Romsets and multi-game compilations fall back to manual selection
        assert(!planRetroAutoSelection("gba", true, {"game.gba"}, actions));
        assert(!planRetroAutoSelection("gba", false, {"a.gba", "b.gba"}, actions));
        assert(!planRetroAutoSelection("gba", false, {"game.part1.rar", "game.part2.rar"}, actions));
        // PS1 cue + multi-track bin + info.txt
        assert(planRetroAutoSelection("ps1", false,
                                      {"Game.cue", "Game (Track 1).bin", "Game (Track 2).bin", "info.txt"},
                                      actions));
        assert((actions == std::vector<uint8_t>{1, 1, 1, 0}));
        // Multi-disc PS1/GC set
        assert(planRetroAutoSelection("ps1", false,
                                      {"FF7 (Disc 1).chd", "FF7 (Disc 2).chd", "readme.nfo"},
                                      actions));
        assert((actions == std::vector<uint8_t>{1, 1, 0}));
        // Unrelated multi-disc / multi-game ISO set falls back
        assert(!planRetroAutoSelection("ps1", false, {"Crash.chd", "Spyro.chd"}, actions));
        // Wii U folder dump
        assert(planRetroAutoSelection("wiiu", false,
                                      {"Game/code/app.rpx", "Game/content/data.bin", "Game/meta/meta.xml", "notes.txt"},
                                      actions));
        assert((actions == std::vector<uint8_t>{1, 1, 1, 0}));
        // PS Vita folder dump
        assert(planRetroAutoSelection("psvita", false,
                                      {"PCSE00001/eboot.bin", "PCSE00001/sce_sys/param.sfo", "cover.png"},
                                      actions));
        assert((actions == std::vector<uint8_t>{1, 1, 0}));

        // PS Vita NoNpDrm with app/ + patch/ collapses to 1 game (app/) rather than 2
        RetroRelease vitaRel;
        vitaRel.platform = "psvita";
        vitaRel.topicId = "7020";
        vitaRel.title = "Vita NoNpDrm Game";
        vitaRel.infoHash = std::string(40, '6');
        RetroRomOperation vitaNoNpOp;
        assert(store.plan(vitaRel, "roms/psvita", "vita3k", vitaNoNpOp, error, true));
        const auto vitaApp = root / "vita-app-eboot.bin";
        const auto vitaAppSfo = root / "vita-app-param.sfo";
        const auto vitaPatch = root / "vita-patch-eboot.bin";
        const auto vitaPatchSfo = root / "vita-patch-param.sfo";
        write(vitaApp, std::string(1024, 'A'));
        write(vitaAppSfo, std::string(256, 'S'));
        write(vitaPatch, std::string(512, 'P'));
        write(vitaPatchSfo, std::string(256, 'S'));
        std::vector<RetroRomInput> vitaInputs = {
            {"Game/app/PCSE01262/eboot.bin", vitaApp.string(), 1024},
            {"Game/app/PCSE01262/sce_sys/param.sfo", vitaAppSfo.string(), 256},
            {"Game/patch/PCSE01262/eboot.bin", vitaPatch.string(), 512},
            {"Game/patch/PCSE01262/sce_sys/param.sfo", vitaPatchSfo.string(), 256},
        };
        assert(store.prepare(vitaNoNpOp.id, vitaInputs, cancelled, vitaNoNpOp, error));
        assert(vitaNoNpOp.ready && vitaNoNpOp.games.size() == 1 && vitaNoNpOp.games[0].selected);
        assert(vitaNoNpOp.games[0].path == "files/Game/app/PCSE01262/eboot.bin");
        assert(store.addSelectedToLibrary(vitaNoNpOp.id, error));
        // NoNpDrm app/ and patch/ stay intact in prepared folder for Vita3K's on-device installer
        // (must not be naively overwritten into ux0/app while still PFS-encrypted).
        const auto preparedVitaDir = root / "sd/roms/psvita" / vitaNoNpOp.id / "files/Game";
        assert(fs::file_size(preparedVitaDir / "app/PCSE01262/eboot.bin") == 1024);
        assert(fs::file_size(preparedVitaDir / "patch/PCSE01262/eboot.bin") == 512);
        assert(!fs::exists(root / "sd/switch/vita3k/vita/ux0/app/PCSE01262/eboot.bin"));
    }

    fs::remove_all(root);
    std::cout << "retro ROM preparation: OK\n";
}
