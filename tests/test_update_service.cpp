#include "app/update_service.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

constexpr const char* Target = "/tmp/pipensx-update-test.nro";

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

void testInstallVerifiesBeforeReplacing() {
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
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
    std::ifstream installed(Target, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(installed)), {});
    assert(bytes == payload);

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
}

} // namespace

int main() {
    testVersionsAndReleaseValidation();
    testInstallVerifiesBeforeReplacing();
    std::puts("update service tests passed");
    return 0;
}
