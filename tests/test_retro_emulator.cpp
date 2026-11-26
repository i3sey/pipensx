#include "app/retro_emulator.hpp"
#include "app/retro_catalog.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace pipensx;
namespace fs = std::filesystem;
static void write(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << body;
}
static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
static std::string makeZip(const std::vector<std::pair<std::string, std::string>>& entries) {
    char tmp[] = "/tmp/pipensx-emu-zip-XXXXXX";
    assert(mkdtemp(tmp));
    const fs::path zipPath = fs::path(tmp) / "pkg.zip";
    archive* writer = archive_write_new();
    assert(writer);
    assert(archive_write_set_format_zip(writer) == ARCHIVE_OK);
    assert(archive_write_open_filename(writer, zipPath.c_str()) == ARCHIVE_OK);
    for (const auto& item : entries) {
        auto* entry = archive_entry_new();
        archive_entry_set_pathname(entry, item.first.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, item.second.size());
        assert(archive_write_header(writer, entry) == ARCHIVE_OK);
        assert(archive_write_data(writer, item.second.data(), item.second.size()) ==
               static_cast<ssize_t>(item.second.size()));
        archive_entry_free(entry);
    }
    assert(archive_write_close(writer) == ARCHIVE_OK);
    assert(archive_write_free(writer) == ARCHIVE_OK);
    std::string out = read(zipPath);
    fs::remove_all(tmp);
    return out;
}
int main(int argc, char** argv) {
    char temporary[] = "/tmp/pipensx-emulator-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path root = temporary;
    const auto target = root / "switch/mGBA/mGBA.nro";
    const auto stage = root / "switch/mGBA/.pipensx-install";
    const auto config = root / "switch/mGBA/config.ini";
    write(config, "user config");
    write(root / "switch/mGBA/game.sav", "user save");
    RetroEmulatorPackage package;
    package.id = "mgba";
    package.name = "mGBA";
    package.version = "0.10.5";
    package.downloadUrl = "https://github.com/mgba-emu/mgba/releases/download/0.10.5/mGBA-0.10.5-switch.7z";
    package.filename = "mGBA-switch.7z";
    package.bytes = 2211306;
    package.archive = true;
    assert(RetroEmulatorInstaller::supports(package));

    // Every catalog console must have at least one emulator profile, and all profiles must be well-formed.
    assert(retroEmulatorProfiles().size() >= 30);
    for (const auto& console : retroConsoles()) {
        const auto emus = emulatorsForConsole(console.id);
        assert(!emus.empty());
    }
    assert(isCustomEmulatorPath("sdmc:/switch/custom/my_emu.nro"));
    assert(!isCustomEmulatorPath("sdmc:/switch/../evil.nro"));
    assert(!isCustomEmulatorPath("roms/game.gba"));
    assert(isValidEmulatorSelection("ps1", "duckstation"));
    assert(isValidEmulatorSelection("ps1", "ra_pcsx_rearmed"));
    assert(!isValidEmulatorSelection("ps1", "mgba"));
    assert(isValidEmulatorSelection("ps1", "sdmc:/switch/custom.nro"));
    bool called = false, cancelDownload = false;
    std::string payload;
    std::atomic<bool> cancelled{false};
    auto fetch = [&](const std::string&, uint64_t, const HomebrewCatalog::Sink& sink,
                     const std::atomic<bool>&, const HomebrewCatalog::Progress&, std::string&) {
        called = true;
        if (cancelDownload) cancelled = true;
        return sink(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    };
    RetroEmulatorInstaller installer(root.string(), fetch);
    std::string path = "unchanged", error, args = "unchanged";
    assert(!installer.detect(path, error) && path == "unchanged");
    assert(mgbaProfile().applicationModeRequired && mgbaProfile().platform == "gba");
    assert(RetroEmulatorInstaller::launchArguments("sdmc:/switch/mGBA/mGBA.nro", "sdmc:/roms/Проверка GBA.GBA", args, error));
    assert(args == "\"sdmc:/switch/mGBA/mGBA.nro\" \"sdmc:/roms/Проверка GBA.GBA\"");
    for (const auto* bad : {"sdmc:/../game.gba", "sdmc:/roms/a\".gba", "sdmc:/roms/game.zip", "/game.gba", "sdmc:/roms/a\n.gba"}) {
        args = "unchanged";
        assert(!RetroEmulatorInstaller::launchArguments("sdmc:/switch/mGBA/mGBA.nro", bad, args, error));
        assert(args == "unchanged");
    }
    auto bumped = package;
    bumped.version = "0.10.6";
    assert(RetroEmulatorInstaller::supports(bumped));
    auto unsupported = package;
    unsupported.id = "unknown";
    assert(!RetroEmulatorInstaller::supports(unsupported));
    assert(!installer.install(unsupported, cancelled, {}, error) && !called);
    unsupported = package;
    unsupported.filename = "../evil.7z";
    assert(!RetroEmulatorInstaller::supports(unsupported));
    assert(!installer.install(unsupported, cancelled, {}, error) && !called);
    unsupported = package;
    unsupported.downloadUrl = "https://example.com/other.7z";
    assert(!installer.install(unsupported, cancelled, {}, error) && !called);
    cancelled = true;
    assert(!installer.install(package, cancelled, {}, error) && !called && error == "Cancelled.");
    cancelled = false;
    payload = "short";
    write(stage / "stale", "user file");
    assert(!installer.install(package, cancelled, {}, error) && !called);
    assert(read(stage / "stale") == "user file");
    write(stage / ".owner", "pipensx-mgba-install-v1\n");
    assert(!installer.install(package, cancelled, {}, error) && called);
    assert(!fs::exists(stage) && !fs::exists(target));
    RetroEmulatorInstaller racing(root.string(), [&](const std::string&, uint64_t,
        const HomebrewCatalog::Sink& sink, const std::atomic<bool>&,
        const HomebrewCatalog::Progress&, std::string&) {
        write(target, "created during download");
        return sink(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    });
    payload.assign(package.bytes, 'X');
    assert(!installer.install(package, cancelled, {}, error) && error.find("SHA-256") != std::string::npos);
    payload.push_back('X');
    assert(!installer.install(package, cancelled, {}, error));
    cancelDownload = true;
    assert(!installer.install(package, cancelled, {}, error) && error == "Cancelled.");
    assert(!fs::exists(stage) && !fs::exists(target));
    cancelDownload = false;
    cancelled = false;
    write(target, "existing broken NRO");
    called = false;
    assert(!installer.install(package, cancelled, {}, error) && !called);
    assert(read(target) == "existing broken NRO");
    assert(!installer.detect(path, error));
    fs::remove(target);
    std::string nro(128, '\0');
    nro.replace(0x10, 4, "NRO0"); nro[0x18] = char(128);
    write(target, nro);
    assert(installer.detect(path, error) && path == "sdmc:/switch/mGBA/mGBA.nro");
    fs::remove(target);
    fs::create_symlink(root / "outside.nro", target);
    assert(!installer.install(package, cancelled, {}, error));
    assert(!fs::exists(root / "outside.nro"));
    fs::remove(target);
    assert(read(config) == "user config" && read(root / "switch/mGBA/game.sav") == "user save");

    // Multi-emulator checks: ZIP extraction ignores stray .nsp, dynamic size == 0 works, allowReplace & uninstall keep configs, Vita3K NoRomOpenEmulator, BIOS check
    {
        std::string fakeNro(256, '\0');
        fakeNro.replace(0x10, 4, "NRO0");
        fakeNro[0x18] = char(128);

        const std::string duckZip = makeZip({
            {"switch/duckstation/duckstation.nro", fakeNro},
            {"duckstation_forwarder.nsp", "STRAY_NSP_SHOULD_BE_IGNORED"},
            {"README.txt", "notes"},
        });
        RetroEmulatorPackage duckPkg = RetroEmulatorInstaller::resolvePackage("duckstation", {});
        duckPkg.bytes = duckZip.size();
        std::string duckError, duckPath, duckArgs;
        payload = duckZip;
        cancelled = false;
        assert(RetroEmulatorInstaller::supports(duckPkg));
        assert(installer.install(duckPkg, cancelled, {}, duckError));
        assert(installer.detect("duckstation", duckPath, duckError) &&
               duckPath == "sdmc:/switch/duckstation/duckstation.nro");
        assert(!fs::exists(root / "duckstation_forwarder.nsp") &&
               !fs::exists(root / "switch/duckstation/duckstation_forwarder.nsp"));
        assert(RetroEmulatorInstaller::launchArguments("duckstation", duckPath,
                                                       "sdmc:/roms/ps1/game.m3u", duckArgs, duckError));
        assert(duckArgs == "\"sdmc:/switch/duckstation/duckstation.nro\" \"sdmc:/roms/ps1/game.m3u\"");

        // Dynamic Libretro core (.zip with bytes == 0) + alternate RA core + allowReplace + safe uninstall
        RetroEmulatorPackage raPkg = RetroEmulatorInstaller::resolvePackage("ra_pcsx_rearmed", {});
        raPkg.bytes = 0; // dynamic nightly size
        const std::string raZip = makeZip({{"pcsx_rearmed_libretro_libnx.nro", fakeNro}});
        payload = raZip;
        const auto raCfg = root / "retroarch/cores/pcsx_rearmed.opt";
        write(raCfg, "core opt");
        assert(installer.install(raPkg, cancelled, {}, duckError));
        std::string raPath, raArgs;
        assert(installer.detect("ra_pcsx_rearmed", raPath, duckError) &&
               raPath == "sdmc:/retroarch/cores/pcsx_rearmed_libretro_libnx.nro");
        assert(RetroEmulatorInstaller::launchArguments("ra_pcsx_rearmed", raPath,
                                                       "sdmc:/roms/ps1/game.m3u", raArgs, duckError));
        assert(raArgs == "\"/retroarch/cores/pcsx_rearmed_libretro_libnx.nro\" \"/roms/ps1/game.m3u\"");
        std::string raMgbaArgs;
        assert(RetroEmulatorInstaller::launchArguments(
            "ra_mgba", "sdmc:/retroarch/cores/mgba_libretro_libnx.nro",
            "sdmc:/roms/gba/Проверка GBA.gba", raMgbaArgs, duckError));
        assert(raMgbaArgs == "\"/retroarch/cores/mgba_libretro_libnx.nro\" \"/roms/gba/Проверка GBA.gba\"");
        std::string customRaArgs;
        assert(RetroEmulatorInstaller::launchArguments(
            "sdmc:/retroarch/cores/custom_libretro_libnx.nro",
            "sdmc:/retroarch/cores/custom_libretro_libnx.nro",
            "sdmc:/roms/snes/Super Metroid.sfc", customRaArgs, duckError));
        assert(customRaArgs == "\"/retroarch/cores/custom_libretro_libnx.nro\" \"/roms/snes/Super Metroid.sfc\"");
        assert(!installer.install(raPkg, cancelled, {}, duckError, false));
        assert(installer.install(raPkg, cancelled, {}, duckError, true)); // allowReplace
        assert(installer.uninstall("ra_pcsx_rearmed", duckError));
        assert(!installer.detect("ra_pcsx_rearmed", raPath, duckError));
        assert(read(raCfg) == "core opt"); // Config preserved on uninstall!

        // Vita3K uses -g "<TITLEID>" for NoNpDrm eboot.bin and opens launcher for .vpk
        std::string vitaArgs;
        assert(RetroEmulatorInstaller::launchArguments("vita3k", "sdmc:/switch/vita3k/Vita3K.nro",
                                                       "sdmc:/roms/psvita/Game.vpk", vitaArgs, duckError));
        assert(vitaArgs == "\"sdmc:/switch/vita3k/Vita3K.nro\"");
        std::string vitaEbootArgs;
        assert(RetroEmulatorInstaller::launchArguments(
            "vita3k", "sdmc:/switch/Vita3K/Vita3K.nro",
            "sdmc:/roms/psvita/psvita-5704778/files/NFS MW/app/PCSB00183/eboot.bin",
            vitaEbootArgs, duckError));
        assert(vitaEbootArgs == "\"sdmc:/switch/Vita3K/Vita3K.nro\" -g \"PCSB00183\"");

        // BIOS checks
        const auto ps1BiosMissing = checkBiosStatus(root.string(), "ps1", "duckstation");
        assert(ps1BiosMissing.required && !ps1BiosMissing.present);
        write(root / "switch/duckstation/bios/scph5501.bin", std::string(512 * 1024, 'B'));
        const auto ps1BiosPresent = checkBiosStatus(root.string(), "ps1", "duckstation");
        assert(ps1BiosPresent.required && ps1BiosPresent.present);

        const auto gbaBios = checkBiosStatus(root.string(), "gba", "mgba");
        assert(!gbaBios.required && gbaBios.present);

        const auto melonBiosMissing = checkBiosStatus(root.string(), "nds", "melonds");
        assert(melonBiosMissing.required && !melonBiosMissing.present &&
               melonBiosMissing.expectedPath == "sdmc:/switch/melonds/");
        write(root / "switch/melonds/bios7.bin", std::string(16 * 1024, '7'));
        write(root / "switch/melonds/bios9.bin", std::string(4 * 1024, '9'));
        write(root / "switch/melonds/firmware.bin", std::string(256 * 1024, 'F'));
        const auto melonBiosPresent = checkBiosStatus(root.string(), "nds", "melonds");
        assert(melonBiosPresent.required && melonBiosPresent.present);

        const auto raMelonBiosMissing = checkBiosStatus(root.string(), "nds", "ra_melonds");
        assert(raMelonBiosMissing.required && !raMelonBiosMissing.present);

        // PS Vita firmware check must not treat Vita3K.nro or an uninstalled .PUP as installed firmware.
        write(root / "switch/Vita3K/Vita3K.nro", fakeNro);
        const auto vitaFwMissing = checkBiosStatus(root.string(), "psvita", "vita3k");
        assert(vitaFwMissing.required && !vitaFwMissing.present);
        write(root / "switch/Vita3K/PSP2UPDAT.PUP", std::string(1024, 'P'));
        const auto vitaFwUninstalled = checkBiosStatus(root.string(), "psvita", "vita3k");
        assert(vitaFwUninstalled.required && !vitaFwUninstalled.present);

        // Rolling Libretro nightly clears stale catalog file_size so nightly size changes succeed.
        RetroEmulatorPackage staleMupen = RetroEmulatorInstaller::resolvePackage("mupen64plus_next", {});
        staleMupen.bytes = 9536707;
        const auto resolvedMupen = RetroEmulatorInstaller::resolvePackage("mupen64plus_next", {staleMupen});
        assert(resolvedMupen.bytes == 0);
        const std::string mupenZip = makeZip({{"mupen64plus_next_libretro_libnx.nro", fakeNro}});
        assert(mupenZip.size() != staleMupen.bytes);
        payload = mupenZip;
        assert(installer.install(staleMupen, cancelled, {}, duckError));
        std::string mupenPath;
        assert(installer.detect("mupen64plus_next", mupenPath, duckError));

        // allowReplace removes stale alternate NRO paths while keeping case-aliased primary NROs.
        const auto altFlycast = root / "switch/flycast/flycast-2.7.nro";
        write(altFlycast, fakeNro);
        RetroEmulatorPackage flyPkg = RetroEmulatorInstaller::resolvePackage("flycast", {});
        flyPkg.bytes = fakeNro.size();
        payload = fakeNro;
        assert(installer.install(flyPkg, cancelled, {}, duckError, true));
        assert(fs::exists(root / "switch/flycast/flycast.nro"));
        assert(!fs::exists(altFlycast));

        RetroEmulatorPackage mgbaReplace = package;
        mgbaReplace.version = "0.10.6";
        mgbaReplace.downloadUrl = "https://github.com/mgba-emu/mgba/releases/download/0.10.6/mGBA.zip";
        mgbaReplace.filename = "mGBA.zip";
        const std::string mgbaZip = makeZip({{"mGBA/mgba.nro", fakeNro}});
        mgbaReplace.bytes = mgbaZip.size();
        payload = mgbaZip;
        assert(installer.install(mgbaReplace, cancelled, {}, duckError, true));
        assert(fs::exists(target));
        assert(installer.install(mgbaReplace, cancelled, {}, duckError, true));
        assert(fs::exists(target));

        // Vita3K firmware & on-device installer helpers
        const auto vitaRoot = root / "switch/Vita3K";
        write(vitaRoot / "PSP2UPDAT.PUP", "pup");
        // Uninstalled .pup alone must NOT count as installed firmware.
        assert(!checkBiosStatus(root.string(), "psvita", "vita3k").present);
        assert(findPendingVitaFirmwarePup(root.string()) == "sdmc:/switch/Vita3K/PSP2UPDAT.PUP");
        write(vitaRoot / "vita/vs0/sys/external/libkernel.suprx", "suprx");
        assert(checkBiosStatus(root.string(), "psvita", "vita3k").present);

        assert(extractVitaTitleIdFromRomPath("sdmc:/roms/psvita/id/files/NFS/app/pcsb00183/eboot.bin") == "PCSB00183");
        assert(extractVitaTitleIdFromRomPath("sdmc:/roms/psvita/id/files/game.vpk").empty());
        assert(resolveVitaInstallTarget("sdmc:/roms/psvita/id/files/NFS/app/PCSB00183/eboot.bin") ==
               "sdmc:/roms/psvita/id/files/NFS");
        assert(resolveVitaInstallTarget("sdmc:/roms/psvita/id/files/game.vpk") ==
               "sdmc:/roms/psvita/id/files/game.vpk");

        const auto ux0App = vitaRoot / "vita/ux0/app/PCSB00183";
        write(ux0App / "eboot.bin", std::string("\xa5\x8b\xf9\x7f" "encrypted", 13));
        write(ux0App / "sce_sys/param.sfo", "sfo");
        assert(!isVitaTitleInstalledDecrypted(root.string(), "PCSB00183"));
        write(ux0App / "eboot.bin", std::string("SCE\0decrypted", 13));
        write(ux0App / "sce_pfs/files.db", "pfs");
        assert(!isVitaTitleInstalledDecrypted(root.string(), "PCSB00183"));
        fs::remove_all(ux0App / "sce_pfs");
        assert(isVitaTitleInstalledDecrypted(root.string(), "PCSB00183"));

        std::string vNro, vArgs, vErr;
        assert(!prepareVita3kInstallLaunch(root.string(), "sdmc:/roms/psvita/id/files/NFS",
                                           "sdmc:/switch/pipensx/pipensx.nro", vNro, vArgs, vErr));
        write(vitaRoot / ".emu/Vita3K.nro", fakeNro);
        assert(prepareVita3kInstallLaunch(root.string(), "sdmc:/roms/psvita/id/files/NFS",
                                          "sdmc:/switch/pipensx/pipensx.nro", vNro, vArgs, vErr));
        assert(vNro == "sdmc:/switch/Vita3K/.emu/Vita3K.nro");
        assert(vArgs == "\"sdmc:/switch/Vita3K/.emu/Vita3K.nro\" --install --return \"sdmc:/switch/pipensx/pipensx.nro\"");
        assert(read(vitaRoot / "install_target.txt") == "sdmc:/roms/psvita/id/files/NFS\n");
    }

    if (argc > 1) {
        payload = read(argv[1]);
        assert(payload.size() == package.bytes);
        if (!installer.install(package, cancelled, {}, error)) { std::cerr << error << '\n'; return 1; }
        assert(installer.detect(path, error));
        assert(fs::file_size(target) == 7268339);
        assert(!fs::exists(stage));
        assert(read(config) == "user config" && read(root / "switch/mGBA/game.sav") == "user save");
        assert(!installer.install(package, cancelled, {}, error));
        fs::remove(target);
        assert(!racing.install(package, cancelled, {}, error));
        assert(read(target) == "created during download");
        assert(!fs::exists(stage));
        std::cout << "Real mGBA package installation and concurrent conflict: OK\n";
    }
    fs::remove_all(root);
    std::cout << "retro emulator: OK\n";
}
