#include "rutracker_client.hpp"

extern "C" {
#include "../core/util.h"
}

#include <curl/curl.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace pipensx {
namespace {

// Windows-1251 high range (0x80-0xBF) to Unicode code points. 0xC0-0xFF map
// linearly onto Cyrillic U+0410..U+044F and are handled programmatically.
// A 0 entry marks an undefined byte.
constexpr std::array<uint16_t, 64> kCp1251HighLow = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, // 80-87
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, // 88-8F
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90-97
    0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, // 98-9F
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, // A0-A7
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, // A8-AF
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, // B0-B7
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, // B8-BF
};

uint32_t cp1251CodePoint(unsigned char byte) {
    if (byte < 0x80)
        return byte;
    if (byte >= 0xC0)
        return 0x0410u + (byte - 0xC0u); // А..я
    return kCp1251HighLow[byte - 0x80];
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp == 0)
        return;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool isDigits(const std::string& s) {
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

std::string urlEncode(const std::string& cp1251) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(cp1251.size() * 3);
    for (unsigned char c : cp1251) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Remove HTML tags from a fragment, keeping their text content.
std::string stripTags(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool inTag = false;
    for (char c : html) {
        if (c == '<')
            inTag = true;
        else if (c == '>')
            inTag = false;
        else if (!inTag)
            out.push_back(c);
    }
    return out;
}

// Decode the small set of HTML entities rutracker emits. Numeric references
// (&#NN; / &#xHH;) are Unicode and produce UTF-8 directly.
std::string decodeEntities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '&') {
            out.push_back(in[i++]);
            continue;
        }
        size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back(in[i++]);
            continue;
        }
        std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos" || ent == "#39") out.push_back('\'');
        else if (ent == "nbsp") out.push_back(' ');
        else if (!ent.empty() && ent[0] == '#') {
            uint32_t cp = 0;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                cp = static_cast<uint32_t>(std::strtoul(ent.c_str() + 2, nullptr, 16));
            else
                cp = static_cast<uint32_t>(std::strtoul(ent.c_str() + 1, nullptr, 10));
            appendUtf8(out, cp);
        } else {
            out.append(in, i, semi - i + 1); // unknown entity: keep verbatim
        }
        i = semi + 1;
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\xC2\xA0");
    if (a == std::string::npos)
        return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// First run of decimal digits appearing after `key` within `row`.
int digitsAfter(const std::string& row, const char* key) {
    size_t p = row.find(key);
    if (p == std::string::npos)
        return 0;
    p += std::strlen(key);
    while (p < row.size() && !std::isdigit(static_cast<unsigned char>(row[p])))
        ++p;
    long value = 0;
    bool any = false;
    while (p < row.size() && std::isdigit(static_cast<unsigned char>(row[p]))) {
        value = value * 10 + (row[p] - '0');
        any = true;
        ++p;
    }
    return any ? static_cast<int>(value) : 0;
}

// Reads the exact byte size from a `data-ts_text="<bytes>"` attribute that
// follows `key` (the size cell carries it).
uint64_t tsTextAfter(const std::string& row, const char* key) {
    size_t p = row.find(key);
    if (p == std::string::npos)
        return 0;
    p = row.find("data-ts_text=\"", p);
    if (p == std::string::npos)
        return 0;
    p += std::strlen("data-ts_text=\"");
    uint64_t value = 0;
    bool any = false;
    while (p < row.size() && std::isdigit(static_cast<unsigned char>(row[p]))) {
        value = value * 10 + static_cast<uint64_t>(row[p] - '0');
        any = true;
        ++p;
    }
    return any ? value : 0;
}

std::string humanSize(uint64_t bytes) {
    if (bytes == 0)
        return {};
    char buf[32];
    fmt_bytes(buf, sizeof(buf), bytes);
    return buf;
}

bool looksLoggedOut(const std::string& body) {
    return body.find("login_password") != std::string::npos &&
           body.find("logout=") == std::string::npos;
}

struct CurlBuffer {
    std::string* out;
};

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<CurlBuffer*>(userdata);
    buf->out->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

RuTrackerClient::RuTrackerClient(std::string rootPath)
    : rootPath_(std::move(rootPath)) {
    configPath_ = rootPath_ + "/rutracker.cfg";
    cookiePath_ = rootPath_ + "/rutracker_cookies.txt";
    loadConfig();
}

void RuTrackerClient::setBaseUrl(const std::string& value) {
    config_.baseUrl = value;
    // Normalise away a trailing slash so we can append "/login.php" etc.
    while (!config_.baseUrl.empty() && config_.baseUrl.back() == '/')
        config_.baseUrl.pop_back();
    loggedIn_ = false;
}

void RuTrackerClient::setUsername(const std::string& value) {
    config_.username = value;
    loggedIn_ = false;
}

void RuTrackerClient::setPassword(const std::string& value) {
    config_.password = value;
    loggedIn_ = false;
}

bool RuTrackerClient::hasCredentials() const {
    return !config_.username.empty() && !config_.password.empty();
}

bool RuTrackerClient::loadConfig() {
    std::ifstream in(configPath_);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "base_url" && !value.empty())
            config_.baseUrl = value;
        else if (key == "username")
            config_.username = value;
        else if (key == "password")
            config_.password = value;
    }
    while (!config_.baseUrl.empty() && config_.baseUrl.back() == '/')
        config_.baseUrl.pop_back();
    return true;
}

bool RuTrackerClient::saveConfig() const {
    std::ofstream out(configPath_, std::ios::trunc);
    if (!out)
        return false;
    out << "base_url=" << config_.baseUrl << "\n";
    out << "username=" << config_.username << "\n";
    out << "password=" << config_.password << "\n";
    return out.good();
}

bool RuTrackerClient::httpRequest(const std::string& url,
                                  const std::string* postFields,
                                  std::string& body, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Unable to initialise HTTP client.";
        return false;
    }
    body.clear();
    CurlBuffer buffer { &body };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookiePath_.c_str());
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, cookiePath_.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/120.0 Safari/537.36");
    if (postFields) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(postFields->size()));
    }
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        error = std::string("Network error: ") + curl_easy_strerror(rc);
        log_msg("[rutracker] %s -> %s\n", url.c_str(), error.c_str());
        return false;
    }
    return true;
}

bool RuTrackerClient::login(std::string& error) {
    if (!hasCredentials()) {
        error = "Set your rutracker login and password first.";
        return false;
    }
    std::string post =
        "login_username=" + urlEncode(utf8ToCp1251(config_.username)) +
        "&login_password=" + urlEncode(utf8ToCp1251(config_.password)) +
        // Submit button value "вход" in cp1251; only its presence matters.
        "&login=%E2%F5%EE%E4";
    std::string body;
    if (!httpRequest(config_.baseUrl + "/login.php", &post, body, error))
        return false;
    if (body.find("cap_sid") != std::string::npos ||
        body.find("captcha") != std::string::npos) {
        error = "rutracker is asking for a captcha; sign in from a browser "
                "once, then try again.";
        loggedIn_ = false;
        return false;
    }
    if (looksLoggedOut(body)) {
        error = "Login failed — check your username and password.";
        loggedIn_ = false;
        return false;
    }
    loggedIn_ = true;
    log_msg("[rutracker] logged in as '%s'\n", config_.username.c_str());
    return true;
}

bool RuTrackerClient::search(const std::string& query,
                             std::vector<RuTrackerResult>& results,
                             std::string& error) {
    results.clear();
    std::string url =
        config_.baseUrl + "/tracker.php?nm=" + urlEncode(utf8ToCp1251(query));
    std::string body;
    if (!httpRequest(url, nullptr, body, error))
        return false;
    if (looksLoggedOut(body)) {
        if (!login(error))
            return false;
        if (!httpRequest(url, nullptr, body, error))
            return false;
        if (looksLoggedOut(body)) {
            error = "Still not signed in after login.";
            return false;
        }
    }
    results = parseSearchResults(body);
    log_msg("[rutracker] search '%s' -> %zu results\n", query.c_str(),
            results.size());
    return true;
}

bool RuTrackerClient::downloadTorrent(const std::string& topicId,
                                      std::vector<uint8_t>& bytes,
                                      std::string& error) {
    std::string url = config_.baseUrl + "/dl.php?t=" + urlEncode(topicId);
    std::string body;
    if (!httpRequest(url, nullptr, body, error))
        return false;

    auto firstNonSpace = body.find_first_not_of(" \t\r\n");
    bool bencoded = firstNonSpace != std::string::npos &&
                    body[firstNonSpace] == 'd';
    if (!bencoded) {
        // Probably an HTML login/error page; try once more after re-login.
        if (login(error)) {
            if (!httpRequest(url, nullptr, body, error))
                return false;
            firstNonSpace = body.find_first_not_of(" \t\r\n");
            bencoded = firstNonSpace != std::string::npos &&
                       body[firstNonSpace] == 'd';
        }
    }
    if (!bencoded) {
        error = "rutracker did not return a .torrent (login or permissions?).";
        return false;
    }
    bytes.assign(body.begin(), body.end());
    return true;
}

std::string RuTrackerClient::cp1251ToUtf8(const std::string& input) {
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char c : input)
        appendUtf8(out, cp1251CodePoint(c));
    return out;
}

std::string RuTrackerClient::utf8ToCp1251(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        unsigned char c = input[i];
        uint32_t cp;
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < input.size()) {
            cp = ((c & 0x1F) << 6) | (input[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < input.size()) {
            cp = ((c & 0x0F) << 12) | ((input[i + 1] & 0x3F) << 6) |
                 (input[i + 2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < input.size()) {
            cp = 0xFFFD; // outside BMP: not representable in cp1251
            i += 4;
        } else {
            cp = c;
            i += 1;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp >= 0x0410 && cp <= 0x044F) {
            out.push_back(static_cast<char>(0xC0 + (cp - 0x0410)));
        } else {
            unsigned char mapped = '?';
            for (size_t j = 0; j < kCp1251HighLow.size(); ++j) {
                if (kCp1251HighLow[j] == cp) {
                    mapped = static_cast<unsigned char>(0x80 + j);
                    break;
                }
            }
            out.push_back(static_cast<char>(mapped));
        }
    }
    return out;
}

std::vector<RuTrackerResult> RuTrackerClient::parseSearchResults(
    const std::string& html) {
    std::vector<RuTrackerResult> results;
    const std::string anchor = "data-topic_id=\"";
    size_t pos = 0;
    while ((pos = html.find(anchor, pos)) != std::string::npos) {
        size_t idStart = pos + anchor.size();
        size_t idEnd = html.find('"', idStart);
        pos = idStart;
        if (idEnd == std::string::npos)
            break;
        std::string topicId = html.substr(idStart, idEnd - idStart);
        if (!isDigits(topicId))
            continue;

        size_t gt = html.find('>', idEnd);
        if (gt == std::string::npos)
            continue;
        size_t aEnd = html.find("</a>", gt);
        if (aEnd == std::string::npos)
            continue;
        std::string title = trim(decodeEntities(
            cp1251ToUtf8(stripTags(html.substr(gt + 1, aEnd - gt - 1)))));
        if (title.empty())
            continue;

        size_t winEnd = html.find(anchor, aEnd);
        if (winEnd == std::string::npos)
            winEnd = std::min(html.size(), aEnd + 4000);
        std::string row = html.substr(aEnd, winEnd - aEnd);

        RuTrackerResult result;
        result.topicId = topicId;
        result.title = title;
        result.sizeBytes = tsTextAfter(row, "tor-size");
        result.sizeText = humanSize(result.sizeBytes);
        result.seeders = digitsAfter(row, "seedmed");
        result.leechers = digitsAfter(row, "leechmed");
        results.push_back(std::move(result));
        pos = winEnd;
    }
    return results;
}

} // namespace pipensx
