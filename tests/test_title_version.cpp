#include "app/installed_title_service.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    using pipensx::formatTitleVersion;
    using pipensx::titleVersionIsNewer;

    assert(formatTitleVersion("0") == "0.0.0");
    assert(formatTitleVersion("65536") == "1.0.0");
    assert(formatTitleVersion("131072") == "2.0.0");
    assert(formatTitleVersion("262400") == "4.1.0");
    assert(formatTitleVersion("").empty());
    assert(formatTitleVersion("junk").empty());
    assert(formatTitleVersion("1.0.0").empty());
    assert(formatTitleVersion("-1").empty());

    assert(titleVersionIsNewer("131072", "65536"));
    assert(!titleVersionIsNewer("65536", "131072"));
    assert(!titleVersionIsNewer("65536", "65536"));
    assert(!titleVersionIsNewer("", "65536"));
    assert(!titleVersionIsNewer("131072", "junk"));

    std::printf("test_title_version: ok\n");
    return 0;
}
