#include "app/retro_forwarder.hpp"
#include "core/sha256.h"
#include <borealis/extern/nlohmann/json.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <unistd.h>
using namespace pipensx;
namespace fs = std::filesystem;
static std::string hash(const std::string &s) {
    uint8_t bytes[32];
    sha256(s.data(), s.size(), bytes);
    std::string out;
    for (auto b : bytes) {
        char v[3];
        snprintf(v, sizeof(v), "%02x", b);
        out += v;
    }
    return out;
}
struct Fake {
    std::map<uint64_t, std::string> installed;
    int builds = 0, installs = 0, removes = 0;
    bool queryFailure = false, buildFailure = false, installFailure = false, commitThenFail = false,
         race = false;
    std::string title = "Pong", path = "sdmc:/roms/Проверка GBA.gba";
    RetroForwarderPlatform platform() {
        return {[this](uint64_t id, std::string &live, std::string &error) {
                    if (queryFailure) {
                        error = "query failed";
                        return false;
                    }
                    live = installed.count(id) ? installed[id] : "";
                    return true;
                },
                [this](uint64_t id, const RetroForwarderTarget &t, RetroForwarderPackage &p,
                       std::string &error) {
                    ++builds;
                    if (buildFailure) {
                        error = "build failed";
                        return false;
                    }
                    p = {};
                    int i = 0;
                    for (const auto &body :
                         {std::to_string(id) + t.nro + t.arguments, t.title, std::to_string(id)}) {
                        const auto h = hash(body).substr(0, 32);
                        p.files.push_back(
                            {h + (i == 2 ? ".cnmt.nca" : ".nca"), {body.begin(), body.end()}});
                        if (i < 2)
                            p.fingerprint += h;
                        ++i;
                    }
                    if (race)
                        installed[id] = "foreign";
                    return true;
                },
                [this](uint64_t id, const RetroForwarderPackage &p, std::string &error) {
                    ++installs;
                    assert(!installed.count(id));
                    if (installFailure) {
                        error = "install failed";
                        return false;
                    }
                    installed[id] = p.fingerprint;
                    if (commitThenFail) {
                        error = "lost commit response";
                        return false;
                    }
                    return true;
                },
                [this](uint64_t id, const std::string &expected, std::string &error) {
                    ++removes;
                    if (installed[id] != expected) {
                        error = "changed";
                        return false;
                    }
                    installed.erase(id);
                    return true;
                }};
    }
    RetroForwarderService::Resolve resolver() {
        return [this](const std::string &, RetroForwarderTarget &t, std::string &error) {
            if (path.empty()) {
                error = "ROM missing";
                return false;
            }
            t = {title, "sdmc:/switch/mGBA/mGBA.nro",
                 "\"sdmc:/switch/mGBA/mGBA.nro\" \"" + path + "\""};
            return true;
        };
    }
};
int main() {
    char tmp[] = "/tmp/pipensx-retro-forwarder-XXXXXX";
    assert(mkdtemp(tmp));
    const fs::path root = tmp;
    const auto game = "gba-123:" + std::string(64, 'a'), other = "gba-124:" + std::string(64, 'b');
    Fake fake;
    RetroForwarderService service(root.string(), fake.resolver(), fake.platform());
    std::string error;
    uint64_t id = 0;
    assert(service.remove(game, error));
    assert(!service.create("invalid", id, error) && !id && fake.builds == 0);
    fake.path.clear();
    assert(!service.create(game, id, error) && fake.builds == 0);
    fake.path = "sdmc:/roms/Проверка GBA.gba";
    const auto first = RetroForwarderService::candidateId(game);
    fake.installed[first] = "foreign";
    assert(service.create(game, id, error) && id != first && fake.installed[first] == "foreign");
    assert((id & 0xffffff0000000000ULL) == 0x0100D70000000000ULL && !(id & 0x1fff));
    const auto stable = id;
    assert(fake.installs == 1 && fake.removes == 0);
    assert(service.create(game, id, error) && id == stable && fake.installs == 1 &&
           fake.removes == 0);
    RetroForwarderService restarted(root.string(), fake.resolver(), fake.platform());
    assert(restarted.create(game, id, error) && id == stable && fake.installs == 1);
    assert(service.create(other, id, error) && id != stable);
    fake.title = "Renamed Pong";
    assert(service.create(game, id, error) && id == stable && fake.removes == 1 &&
           fake.installs == 3);
    const auto owned = fake.installed[stable];
    fake.installed[stable] = "foreign replacement";
    const auto before = fake.removes;
    assert(!service.create(game, id, error) && !id && fake.removes == before);
    assert(!service.remove(game, error) && fake.removes == before);
    fake.installed[stable] = owned;
    fake.queryFailure = true;
    assert(!service.remove(game, error) && fake.removes == before);
    fake.queryFailure = false;
    assert(service.remove(game, error) && !fake.installed.count(stable));
    assert(service.remove(game, error));
    assert(service.create(game, id, error) && id == stable);
    fake.title = "Next title";
    fake.installFailure = true;
    assert(!service.create(game, id, error) && !id && !fake.installed.count(stable));
    fake.installFailure = false;
    fake.commitThenFail = true;
    assert(!service.create(game, id, error) && fake.installed.count(stable));
    const auto installs = fake.installs;
    fake.commitThenFail = false;
    assert(service.create(game, id, error) && id == stable && fake.installs == installs);
    fake.title = "Race title";
    fake.race = true;
    const auto removes = fake.removes;
    assert(!service.create(game, id, error) && fake.removes == removes &&
           fake.installed[stable] == "foreign");
    fake.race = false;
    assert(!service.remove(game, error) && fake.removes == removes);
    fake.installed.erase(stable);
    assert(service.create(game, id, error) && id == stable);
    fake.buildFailure = true;
    assert(!service.create(game, id, error));
    fake.buildFailure = false;
    fs::path receipt;
    for (const auto &file : fs::directory_iterator(root / "retro/forwarders"))
        if (file.path().extension() == ".json") {
            std::ifstream in(file.path());
            nlohmann::json j;
            in >> j;
            if (j["game"] == game)
                receipt = file.path();
        }
    assert(!receipt.empty());
    fs::copy_file(receipt, receipt.string() + ".previous", fs::copy_options::overwrite_existing);
    std::ofstream(receipt) << "corrupt";
    assert(service.create(game, id, error) && id == stable);
    fs::remove(receipt);
    fs::create_symlink(root / "outside.json", receipt);
    const auto untouched = fake.removes;
    assert(!service.create(game, id, error) && fake.removes == untouched);
    fs::remove(receipt);
    std::ofstream(receipt) << "corrupt";
    std::ofstream(receipt.string() + ".previous") << "corrupt";
    assert(!service.remove(game, error) && fake.removes == untouched);

    // Multi-console + custom NRO path + hasShortcut + remove clears active status
    const auto ps1Game = "ps1-456:" + std::string(64, 'c');
    fake.title = "Castlevania PS1";
    fake.path = "sdmc:/roms/ps1/ps1-456/game.m3u";
    Fake ps1Fake;
    ps1Fake.title = "Castlevania PS1";
    ps1Fake.path = fake.path;
    RetroForwarderService ps1Service(
        (root / "ps1").string(),
        [&](const std::string&, RetroForwarderTarget& t, std::string&) {
            t = {ps1Fake.title, "sdmc:/switch/duckstation/duckstation.nro",
                 "\"sdmc:/switch/duckstation/duckstation.nro\" \"" + ps1Fake.path + "\"",
                 {0xff, 0xd8, 0xff}};
            return true;
        },
        ps1Fake.platform());
    uint64_t ps1Tid = 0;
    assert(ps1Service.create(ps1Game, ps1Tid, error) && ps1Tid != 0);
    assert(ps1Service.hasShortcut(ps1Game));
    assert(ps1Service.remove(ps1Game, error));
    assert(!ps1Service.hasShortcut(ps1Game));

    const auto importGame = "import-snes-0123456789ab:" + std::string(64, 'd');
    assert(ps1Service.create(importGame, ps1Tid, error) && ps1Service.hasShortcut(importGame));

    fs::remove_all(root);
    std::cout << "retro forwarder: OK\n";
}
