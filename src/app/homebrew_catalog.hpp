#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pipensx {

struct HomebrewFile {
    std::string path;
    bool preserve = false;
};

struct HomebrewPackage {
    std::string name, title, author, version, category, description, changelog;
    std::string downloadUrl, downloadType, sha256, iconUrl, installedVersion;
    std::string installedPath;
    uint64_t bytes = 0, extractedBytes = 0;
    std::vector<HomebrewFile> files;
    std::vector<std::string> dependencies;
    bool systemModule = false;
};

class HomebrewCatalog {
public:
    using Sink = std::function<bool(const uint8_t*, size_t)>;
    using Progress = std::function<void(uint64_t, uint64_t)>;
    using Fetcher = std::function<bool(const std::string&, uint64_t, const Sink&,
        const std::atomic<bool>&, const Progress&, std::string&)>;

    HomebrewCatalog(std::string appRoot, std::string sdRoot, Fetcher fetcher = {});
    bool load(std::vector<HomebrewPackage>& packages, const std::string& language,
              std::string& error) const;
    bool loadInstalled(std::vector<HomebrewPackage>& packages, const std::string& language,
                       std::string& error) const;
    bool refresh(std::vector<HomebrewPackage>& packages, const std::string& language,
                 const std::string& sourceUrl, const std::atomic<bool>& cancelled,
                 std::string& error) const;
    bool install(const HomebrewPackage& package, const std::atomic<bool>& cancelled,
                 const Progress& progress, std::string& error) const;
    bool uninstall(const HomebrewPackage& package, std::string& error) const;
    static bool parse(const std::string& body, const std::string& language,
                      const std::string& baseUrl, std::vector<HomebrewPackage>& packages,
                      std::string& error);

private:
    void attachInstalled(std::vector<HomebrewPackage>& packages) const;
    bool recover(std::string& error) const;
    std::string root_, sdRoot_;
    Fetcher fetcher_;
    mutable std::mutex mutex_;
};

std::string homebrewMonogram(const std::string& title);
bool isHomebrewUpdateAvailable(const std::string& installedVersion,
                               const std::string& latestVersion);

} // namespace pipensx
