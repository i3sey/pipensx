#include "app/game_update_service.hpp"
#include "app/installed_title_service.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {

using pipensx::GameUpdateResult;
using pipensx::GameUpdateService;
using pipensx::GameUpdateState;
using pipensx::InstalledTitle;
using pipensx::IUpdateMetadataSource;

constexpr const char* StatePath = "/tmp/pipensx-game-updates.json";

// Test double for the metadata index: titleId -> candidate versions.
// Versions are decimal title versions ("0", "131072"), matching the [vN]
// tags the generator parses from release file names.
class FakeSource : public IUpdateMetadataSource {
public:
    bool collectLatestVersions(const std::string& titleId,
                               std::vector<std::string>& out) const override {
        auto it = versions_.find(titleId);
        if (it == versions_.end())
            return false;
        out.insert(out.end(), it->second.begin(), it->second.end());
        return true;
    }

    void set(const std::string& titleId, std::vector<std::string> versions) {
        versions_[titleId] = std::move(versions);
    }

private:
    std::map<std::string, std::vector<std::string>> versions_;
};

void reset() {
    unlink(StatePath);
    unlink((std::string(StatePath) + ".tmp").c_str());
}

InstalledTitle title(const std::string& titleId, const std::string& version) {
    InstalledTitle t;
    t.titleId = titleId;
    t.version = version;
    return t;
}

// --- state machine ---

void testSourceUnknownWhenNoEntryOrEmptyVersion() {
    FakeSource source;
    GameUpdateService service(&source, StatePath);

    std::string saveError;
    GameUpdateResult noEntry =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(noEntry.state == GameUpdateState::SourceUnknown);
    assert(noEntry.foundVersion.empty());

    source.set("0100000000000001", {});
    GameUpdateResult emptyVersion =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(emptyVersion.state == GameUpdateState::SourceUnknown);

    source.set("0100000000000001", {"", ""});
    GameUpdateResult blankCandidates =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(blankCandidates.state == GameUpdateState::SourceUnknown);

    reset();
}

void testNullSourceReportsSourceUnknown() {
    GameUpdateService service(nullptr, StatePath);
    std::string saveError;
    GameUpdateResult result =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(result.state == GameUpdateState::SourceUnknown);
    reset();
}

void testNoInstalledVersionIsCheckError() {
    FakeSource source;
    source.set("0100000000000001", {"131072"});
    GameUpdateService service(&source, StatePath);
    std::string saveError;
    GameUpdateResult result = service.checkOne("0100000000000001", "", saveError);
    assert(result.state == GameUpdateState::CheckError);
    assert(result.currentVersion.empty());
    assert(result.foundVersion == "131072");
    reset();
}

void testInstalledPatchVersionString() {
    std::unordered_map<uint64_t, uint32_t> patches;
    const uint64_t app = 0x0100AAAA00000000ULL;
    assert(pipensx::installedPatchVersionString(app, patches, false).empty());
    assert(pipensx::installedPatchVersionString(app, patches, true) == "0");
    patches[app | 0x800ULL] = 131072;
    assert(pipensx::installedPatchVersionString(app, patches, true) ==
           "131072");
    // Incomplete scan must not invent "0" even when a partial map exists.
    assert(pipensx::installedPatchVersionString(app, patches, false).empty());
}

void testTitleIdParsing() {
    uint64_t value = 0;
    assert(pipensx::InstalledTitleService::parseTitleId(
        "0100AAAA00000800", value));
    assert(value == 0x0100AAAA00000800ULL);
    assert(pipensx::InstalledTitleService::parseTitleId(
        "0100aaaa00000800", value));
    assert(value == 0x0100AAAA00000800ULL);
    assert(!pipensx::InstalledTitleService::parseTitleId(
        "0100AAAA0000080", value));
    assert(!pipensx::InstalledTitleService::parseTitleId(
        "0100AAAA0000080Z", value));
}

void testUpdateAvailableAndUpToDate() {
    FakeSource source;
    source.set("0100000000000001", {"131072"});
    GameUpdateService service(&source, StatePath);

    std::string saveError;
    GameUpdateResult newer =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(newer.state == GameUpdateState::UpdateAvailable);
    assert(newer.foundVersion == "131072");
    assert(newer.currentVersion == "65536");

    GameUpdateResult same =
        service.checkOne("0100000000000001", "131072", saveError);
    assert(same.state == GameUpdateState::UpToDate);

    GameUpdateResult olderFound =
        service.checkOne("0100000000000001", "262144", saveError);
    assert(olderFound.state == GameUpdateState::UpToDate);

    // No patch installed: installed version is 0, any published patch is
    // an update.
    GameUpdateResult fromZero =
        service.checkOne("0100000000000001", "0", saveError);
    assert(fromZero.state == GameUpdateState::UpdateAvailable);
    reset();
}

void testNonNumericVersionsAreCheckError() {
    FakeSource source;
    GameUpdateService service(&source, StatePath);
    std::string saveError;

    source.set("0100000000000001", {"1.2.3"});
    GameUpdateResult oldStyleCandidate =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(oldStyleCandidate.state == GameUpdateState::CheckError);

    source.set("0100000000000001", {"v131072"});
    GameUpdateResult vPrefixed =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(vPrefixed.state == GameUpdateState::CheckError);

    source.set("0100000000000001", {"131072"});
    GameUpdateResult badCurrent =
        service.checkOne("0100000000000001", "1.0", saveError);
    assert(badCurrent.state == GameUpdateState::CheckError);
    reset();
}

// --- max aggregation across bundles/regions ---

void testMaxAggregation() {
    FakeSource source;
    source.set("0100000000000001", {"65536", "131072", "98304"});
    GameUpdateService service(&source, StatePath);
    std::string saveError;
    GameUpdateResult result =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(result.state == GameUpdateState::UpdateAvailable);
    assert(result.foundVersion == "131072");

    // A non-numeric candidate must not shadow the numeric max.
    source.set("0100000000000001", {"junk", "65536"});
    GameUpdateResult mixed =
        service.checkOne("0100000000000001", "0", saveError);
    assert(mixed.state == GameUpdateState::UpdateAvailable);
    assert(mixed.foundVersion == "65536");

    // Only non-numeric candidates: a source data bug, not "no source".
    source.set("0100000000000001", {"junk", "garbage"});
    GameUpdateResult allJunk =
        service.checkOne("0100000000000001", "65536", saveError);
    assert(allJunk.state == GameUpdateState::CheckError);
    reset();
}

// --- checkAll + stale ---

void testCheckAllAndStale() {
    FakeSource source;
    source.set("0100000000000001", {"131072"});
    source.set("0100000000000002", {"65536"});
    GameUpdateService service(&source, StatePath);

    std::vector<InstalledTitle> installed = {
        title("0100000000000001", "65536"),
        title("0100000000000002", "65536"),
        title("0100000000000003", "65536"), // no source
    };
    std::string saveError;
    service.checkAll(installed, /*installedGeneration=*/7,
                     /*metadataRefreshMs=*/12345, saveError);
    assert(saveError.empty());

    assert(service.results().size() == 3);
    const GameUpdateResult* r1 = service.find("0100000000000001");
    const GameUpdateResult* r3 = service.find("0100000000000003");
    assert(r1 && r1->state == GameUpdateState::UpdateAvailable);
    assert(r3 && r3->state == GameUpdateState::SourceUnknown);

    // same generations -> not stale
    assert(!service.stale(7, 12345));
    // installed set changed -> stale
    assert(service.stale(8, 12345));
    // metadata index refreshed -> stale
    assert(service.stale(7, 99999));
    reset();
}

void testStaleFalseBeforeAnyCheck() {
    FakeSource source;
    GameUpdateService service(&source, StatePath);
    assert(!service.stale(1, 1));
    reset();
}

// --- persistence ---

void testSaveLoadRoundTrip() {
    FakeSource source;
    source.set("0100000000000001", {"131072"});
    GameUpdateService service(&source, StatePath);

    std::string saveError;
    service.checkAll({title("0100000000000001", "65536")}, 3, 555, saveError);
    assert(saveError.empty());

    GameUpdateService reloaded(&source, StatePath);
    std::string loadError;
    assert(reloaded.load(loadError));
    assert(reloaded.results().size() == 1);
    const GameUpdateResult* r = reloaded.find("0100000000000001");
    assert(r && r->state == GameUpdateState::UpdateAvailable);
    assert(r->foundVersion == "131072");
    assert(r->currentVersion == "65536");
    assert(!reloaded.stale(3, 555));
    assert(reloaded.stale(3, 556));
    assert(reloaded.lastCheckedAt() != 0);
    reset();
}

void testIgnoredPersistsAndSkipsAvailableCount() {
    FakeSource source;
    source.set("0100000000000001", {"131072"});
    source.set("0100000000000002", {"131072"});
    GameUpdateService service(&source, StatePath);

    std::vector<InstalledTitle> installed = {
        title("0100000000000001", "65536"),
        title("0100000000000002", "65536"),
    };
    std::string saveError;
    service.checkAll(installed, 1, 1, saveError);
    assert(saveError.empty());
    assert(service.availableCount(installed) == 2);

    service.setIgnored("0100000000000001", true, saveError);
    assert(saveError.empty());
    assert(service.isIgnored("0100000000000001"));
    assert(!service.isIgnored("0100000000000002"));
    assert(service.availableCount(installed) == 1);
    assert(service.find("0100000000000001")->state ==
           GameUpdateState::UpdateAvailable);

    service.checkAll(installed, 1, 1, saveError);
    assert(service.isIgnored("0100000000000001"));
    assert(service.availableCount(installed) == 1);

    GameUpdateService reloaded(&source, StatePath);
    std::string loadError;
    assert(reloaded.load(loadError));
    assert(reloaded.isIgnored("0100000000000001"));
    assert(reloaded.availableCount(installed) == 1);

    reloaded.setIgnored("0100000000000001", false, saveError);
    assert(!reloaded.isIgnored("0100000000000001"));
    assert(reloaded.availableCount(installed) == 2);
    reset();
}

void testLoadMissingFileIsEmpty() {
    FakeSource source;
    GameUpdateService service(&source, StatePath);
    std::string error;
    assert(service.load(error));
    assert(service.results().empty());
    assert(service.lastCheckedAt() == 0);
    reset();
}

void testLoadCorruptFileFails() {
    FakeSource source;
    {
        std::ofstream output(StatePath, std::ios::binary | std::ios::trunc);
        output << "not json";
    }
    GameUpdateService service(&source, StatePath);
    std::string error;
    assert(!service.load(error));
    reset();
}

void testUnknownStateStringsAreDroppedOnLoad() {
    FakeSource source;
    {
        std::ofstream output(StatePath, std::ios::binary | std::ios::trunc);
        output << "{\"version\":1,\"installed_generation\":1,"
                  "\"metadata_refresh_ms\":1,\"last_checked_at\":1,"
                  "\"results\":[{\"title_id\":\"0100000000000001\","
                  "\"state\":\"nonsense\"}]}\n";
    }
    GameUpdateService service(&source, StatePath);
    std::string error;
    assert(service.load(error));
    assert(service.find("0100000000000001") == nullptr);
    reset();
}

} // namespace

int main() {
    reset();
    testSourceUnknownWhenNoEntryOrEmptyVersion();
    testNullSourceReportsSourceUnknown();
    testNoInstalledVersionIsCheckError();
    testInstalledPatchVersionString();
    testTitleIdParsing();
    testUpdateAvailableAndUpToDate();
    testNonNumericVersionsAreCheckError();
    testMaxAggregation();
    testCheckAllAndStale();
    testStaleFalseBeforeAnyCheck();
    testSaveLoadRoundTrip();
    testIgnoredPersistsAndSkipsAvailableCount();
    testLoadMissingFileIsEmpty();
    testLoadCorruptFileFails();
    testUnknownStateStringsAreDroppedOnLoad();
    reset();
    std::printf("test_game_update_service: all passed\n");
    return 0;
}
