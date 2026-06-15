#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

struct RuTrackerResult {
    std::string topicId;
    std::string title;
    std::string sizeText;
    uint64_t sizeBytes = 0;
    int seeders = 0;
    int leechers = 0;
};

struct RuTrackerConfig {
    std::string baseUrl = "https://rutracker.org/forum";
    std::string username;
    std::string password;
};

// Minimal rutracker.org client: logs in with a forum account (cookie session
// persisted to the SD card), runs a search, and downloads a .torrent. All
// network calls block and must run off the UI thread (see brls::async).
class RuTrackerClient {
public:
    explicit RuTrackerClient(std::string rootPath);

    const RuTrackerConfig& config() const { return config_; }
    void setBaseUrl(const std::string& value);
    void setUsername(const std::string& value);
    void setPassword(const std::string& value);
    bool saveConfig() const;
    bool hasCredentials() const;

    // Blocking network operations.
    bool login(std::string& error);
    bool search(const std::string& query,
                std::vector<RuTrackerResult>& results, std::string& error);
    bool downloadTorrent(const std::string& topicId,
                         std::vector<uint8_t>& bytes, std::string& error);

    // Pure, network-free helpers (unit-tested on PC).
    static std::string cp1251ToUtf8(const std::string& input);
    static std::string utf8ToCp1251(const std::string& input);
    static std::vector<RuTrackerResult> parseSearchResults(
        const std::string& html);

private:
    bool loadConfig();
    // Performs a GET (postFields == nullptr) or POST request, following
    // redirects and reusing the persisted cookie jar. Returns the body.
    bool httpRequest(const std::string& url, const std::string* postFields,
                     std::string& body, std::string& error);

    RuTrackerConfig config_;
    std::string rootPath_;
    std::string configPath_;
    std::string cookiePath_;
    bool loggedIn_ = false;
};

} // namespace pipensx
