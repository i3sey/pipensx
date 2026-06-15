#include "app/rutracker_client.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using pipensx::RuTrackerClient;
using pipensx::RuTrackerResult;

namespace {

void testCp1251RoundTrip() {
    // "Игра" encoded in Windows-1251.
    const std::string cp1251 = "\xC8\xE3\xF0\xE0";
    const std::string utf8 = "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0";
    assert(RuTrackerClient::cp1251ToUtf8(cp1251) == utf8);
    assert(RuTrackerClient::utf8ToCp1251(utf8) == cp1251);

    // ASCII passes through untouched, "№" maps to byte 0xB9.
    assert(RuTrackerClient::cp1251ToUtf8("Hi!") == "Hi!");
    assert(RuTrackerClient::cp1251ToUtf8("\xB9") == "\xE2\x84\x96"); // U+2116
    std::cout << "cp1251 round-trip OK\n";
}

void testParseSearchResults() {
    // Markup mirroring rutracker's tracker.php result rows (cp1251 titles).
    const std::string html =
        "<table><tr class=\"tCenter hl-tr\">"
        "<td class=\"row1\"><a class=\"med tLink ts-text bold\" "
        "data-topic_id=\"123456\" href=\"viewtopic.php?t=123456\">"
        "\xC8\xE3\xF0\xE0 &amp; More [NSP]</a></td>"
        "<td class=\"tor-size\" data-ts_text=\"331137079\">"
        "<a class=\"small tr-dl dl-stub\" href=\"dl.php?t=123456\">"
        "315&nbsp;MB&nbsp;&#8595;</a></td>"
        "<td class=\"row4 nowrap\"><b class=\"seedmed\">42</b></td>"
        "<td class=\"row4 leechmed bold\">7</td>"
        "</tr>"
        "<tr class=\"tCenter hl-tr\">"
        "<td class=\"row1\"><a class=\"med tLink ts-text bold\" "
        "data-topic_id=\"789\" href=\"viewtopic.php?t=789\">"
        "Second Title</a></td>"
        "<td class=\"tor-size\" data-ts_text=\"1073741824\">"
        "<a class=\"small tr-dl dl-stub\" href=\"dl.php?t=789\">"
        "1&nbsp;GB</a></td>"
        "<td class=\"row4 nowrap\"><b class=\"seedmed\">0</b></td>"
        "<td class=\"row4 leechmed bold\">3</td>"
        "</tr></table>";

    std::vector<RuTrackerResult> results =
        RuTrackerClient::parseSearchResults(html);
    assert(results.size() == 2);

    assert(results[0].topicId == "123456");
    // "Игра & More [NSP]" with the Cyrillic part converted to UTF-8.
    assert(results[0].title == "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0 & More [NSP]");
    assert(results[0].sizeBytes == 331137079ULL);
    assert(results[0].seeders == 42);
    assert(results[0].leechers == 7);
    assert(!results[0].sizeText.empty());

    assert(results[1].topicId == "789");
    assert(results[1].title == "Second Title");
    assert(results[1].sizeBytes == 1073741824ULL);
    assert(results[1].seeders == 0);
    assert(results[1].leechers == 3);

    // Empty / non-result HTML yields nothing rather than crashing.
    assert(RuTrackerClient::parseSearchResults("<html>no rows</html>").empty());
    std::cout << "parseSearchResults OK (" << results.size() << " rows)\n";
}

} // namespace

int main() {
    testCp1251RoundTrip();
    testParseSearchResults();
    std::cout << "all rutracker tests passed\n";
    return 0;
}
