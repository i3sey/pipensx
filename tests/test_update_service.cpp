#include "app/update_service.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* Target = "/tmp/pipensx-update-test.nro";

bool emulateSwitchRename = false;

extern "C" int __real_rename(const char* oldPath, const char* newPath);

extern "C" int __wrap_rename(const char* oldPath, const char* newPath) {
    if (emulateSwitchRename) {
        if (std::strcmp(oldPath, Target) == 0) {
            errno = EACCES;
            return -1;
        }
        if (std::strcmp(newPath, Target) == 0 && access(Target, F_OK) == 0) {
            errno = EEXIST;
            return -1;
        }
    }
    return __real_rename(oldPath, newPath);
}

std::string releaseJson(const std::string& version, bool checksum = true) {
    return "{\"draft\":false,\"prerelease\":false,\"tag_name\":\"" +
           version + "\",\"assets\":[{\"name\":\"pipensx.nro\","
           "\"browser_download_url\":\"https://github.com/i3sey/pipensx/"
           "releases/download/" + version + "/pipensx.nro\"}" +
           (checksum ? ",{\"name\":\"pipensx.nro.sha256\","
            "\"browser_download_url\":\"https://github.com/i3sey/pipensx/"
            "releases/download/" + version + "/pipensx.nro.sha256\"}" : "") +
           "]}";
}

void write(const std::string& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
}

void testVersionsAndReleaseValidation() {
    assert(pipensx::UpdateService::isNewerVersion("v1.2.3", "1.2.2"));
    assert(!pipensx::UpdateService::isNewerVersion("1.2.3", "1.2.3"));
    assert(!pipensx::UpdateService::isNewerVersion("next", "1.2.3"));
    pipensx::ReleaseInfo release;
    std::string error;
    assert(pipensx::UpdateService::parseRelease(releaseJson("v1.2.3"),
                                                release, error));
    assert(release.version == "v1.2.3");
    assert(!pipensx::UpdateService::parseRelease(releaseJson("v1.2.3", false),
                                                 release, error));
}

void testStagedLaunchDetectionUsesExecutablePath() {
    pipensx::UpdateService service(Target);
    assert(service.isStagedLaunch({service.helperPath()}));
    assert(service.isStagedLaunch({
        service.helperPath() + " --finish-update"}));
    assert(service.isStagedLaunch({Target, "--finish-update"}));
    assert(!service.isStagedLaunch({service.stagedPath()}));
    assert(!service.isStagedLaunch({Target}));
}

void testStagedReadyRecoversInterruptedRestart() {
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/pipensx-update-test.nro.updater");
    write(Target, "old");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    pipensx::UpdateService service(Target,
        [checksum](const std::string&, size_t, std::string& body,
                   std::string&) { body = checksum + "  pipensx.nro\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; });
    assert(!service.stagedReady());
    pipensx::ReleaseInfo release{"v1.2.3",
        "https://github.com/i3sey/pipensx/releases/download/v1.2.3/pipensx.nro",
        "https://github.com/i3sey/pipensx/releases/download/v1.2.3/pipensx.nro.sha256"};
    std::string error;
    assert(service.install(release, error));
    // The app quit before chain-loading the helper: the staged download must
    // still be recognized as complete so the next launch can finalize it.
    assert(service.stagedReady());
    write("/tmp/pipensx-update-test.nro.update", "corrupted");
    assert(!service.stagedReady());
    write("/tmp/pipensx-update-test.nro.update", payload);
    assert(service.stagedReady());
    assert(service.finalizeStaged(error));
    assert(!service.stagedReady());
    std::ifstream finalized(Target, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(finalized)), {});
    assert(bytes == payload);
    finalized.close();
    service.discardStaged();
    unlink(Target);
}

void testInstallVerifiesBeforeStaging() {
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/pipensx-update-test.nro.updater");
    write(Target, "old");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    pipensx::UpdateService service(Target,
        [checksum](const std::string&, size_t, std::string& body,
                   std::string&) { body = checksum + "  pipensx.nro\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; });
    pipensx::ReleaseInfo release{"v1.2.3",
        "https://github.com/i3sey/pipensx/releases/download/v1.2.3/pipensx.nro",
        "https://github.com/i3sey/pipensx/releases/download/v1.2.3/pipensx.nro.sha256"};
    std::string error;
    assert(service.install(release, error));
    std::ifstream installed("/tmp/pipensx-update-test.nro.update",
                            std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(installed)), {});
    assert(bytes == payload);
    std::ifstream current(Target, std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(current)), {});
    assert(bytes == "old");
    std::ifstream marker("/tmp/pipensx-update-test.nro.update.sha256");
    bytes.assign((std::istreambuf_iterator<char>(marker)), {});
    assert(bytes == checksum + "\n");
    std::ifstream helper(service.helperPath(), std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(helper)), {});
    assert(bytes == "old");
    installed.close();
    current.close();
    marker.close();
    helper.close();
    assert(service.finalizeStaged(error));
    std::ifstream finalized(Target, std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(finalized)), {});
    assert(bytes == payload);
    finalized.close();
    service.discardStaged();
    assert(access("/tmp/pipensx-update-test.nro.update", F_OK) != 0);
    assert(access(service.helperPath().c_str(), F_OK) != 0);

    write(Target, "old");
    pipensx::UpdateService wrongChecksum(Target,
        [](const std::string&, size_t, std::string& body,
           std::string&) { body = "0000000000000000000000000000000000000000000000000000000000000000\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; });
    assert(!wrongChecksum.install(release, error));
    std::ifstream preserved(Target, std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(preserved)), {});
    assert(bytes == "old");
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/pipensx-update-test.nro.updater");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
}

void testTransientNetworkFailuresAreRetried() {
    int checkAttempts = 0;
    pipensx::UpdateService checkService(Target,
        [&checkAttempts](const std::string&, size_t, std::string& body,
                         std::string& error) {
            ++checkAttempts;
            if (checkAttempts < 3) {
                error = "Update network error: Timeout was reached";
                return false;
            }
            body = releaseJson("v9.9.9");
            return true;
        });
    const auto result = checkService.check();
    assert(result.ok);
    assert(checkAttempts == 3);

    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    write(Target, "old");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    int checksumAttempts = 0;
    int downloadAttempts = 0;
    pipensx::UpdateService installService(Target,
        [&checksumAttempts, checksum](const std::string&, size_t,
                                      std::string& body,
                                      std::string& error) {
            ++checksumAttempts;
            if (checksumAttempts < 3) {
                error = "Update network error: Timeout was reached";
                return false;
            }
            body = checksum + "  pipensx.nro\n";
            return true;
        },
        [&downloadAttempts, payload](const std::string&,
                                     const std::string& path, size_t,
                                     std::string& error) {
            ++downloadAttempts;
            if (downloadAttempts < 3) {
                error = "Update download failed: Timeout was reached";
                return false;
            }
            write(path, payload);
            return true;
        });
    pipensx::ReleaseInfo release{"v9.9.9",
        "https://github.com/i3sey/pipensx/releases/download/v9.9.9/pipensx.nro",
        "https://github.com/i3sey/pipensx/releases/download/v9.9.9/pipensx.nro.sha256"};
    std::string error;
    assert(installService.install(release, error));
    assert(checksumAttempts == 3);
    assert(downloadAttempts == 3);
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
}

void testInstallNeverTouchesRunningNro() {
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/pipensx-update-test.nro.updater");
    unlink("/tmp/pipensx-update-test.nro.previous");
    write(Target, "old");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    pipensx::UpdateService service(Target,
        [checksum](const std::string&, size_t, std::string& body,
                   std::string&) { body = checksum + "  pipensx.nro\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; });
    pipensx::ReleaseInfo release{"v1.2.3",
        "https://github.com/i3sey/pipensx/releases/download/v1.2.3/pipensx.nro",
        "https://github.com/i3sey/pipensx/releases/download/v1.2.3/pipensx.nro.sha256"};
    std::string error;
    emulateSwitchRename = true;
    const bool installed = service.install(release, error);
    emulateSwitchRename = false;
    assert(installed);
    std::ifstream result(Target, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(result)), {});
    assert(bytes == "old");
    std::ifstream staged("/tmp/pipensx-update-test.nro.update",
                         std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(staged)), {});
    assert(bytes == payload);
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/pipensx-update-test.nro.updater");
    unlink("/tmp/pipensx-update-test.nro.previous");
}

} // namespace

int main() {
    testVersionsAndReleaseValidation();
    testStagedLaunchDetectionUsesExecutablePath();
    testStagedReadyRecoversInterruptedRestart();
    testInstallVerifiesBeforeStaging();
    testTransientNetworkFailuresAreRetried();
    testInstallNeverTouchesRunningNro();
    std::puts("update service tests passed");
    return 0;
}
