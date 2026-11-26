#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pipensx {
struct RetroForwarderTarget {
    std::string title, nro, arguments;
    std::vector<uint8_t> iconJpeg = {};
};
struct RetroForwarderFile {
    std::string name;
    std::vector<uint8_t> bytes;
};
struct RetroForwarderPackage {
    std::vector<RetroForwarderFile> files;
    std::string fingerprint;
};
struct RetroForwarderPlatform {
    std::function<bool(uint64_t, std::string &, std::string &)> inspect;
    std::function<bool(uint64_t, const RetroForwarderTarget &, RetroForwarderPackage &,
                       std::string &)>
        build;
    std::function<bool(uint64_t, const RetroForwarderPackage &, std::string &)> install;
    std::function<bool(uint64_t, const std::string &, std::string &)> remove;
};
RetroForwarderPlatform retroForwarderPlatform(const std::string &appRoot);
class RetroForwarderService {
  public:
    using Resolve = std::function<bool(const std::string &, RetroForwarderTarget &, std::string &)>;
    RetroForwarderService(std::string appRoot, Resolve resolve, RetroForwarderPlatform platform);
    bool create(const std::string &gameId, uint64_t &titleId, std::string &error) const;
    bool remove(const std::string &gameId, std::string &error) const;
    bool hasShortcut(const std::string &gameId) const;
    static uint64_t candidateId(const std::string &gameId, unsigned probe = 0);
    static std::string titleIdText(uint64_t titleId);

  private:
    std::string root_;
    Resolve resolve_;
    RetroForwarderPlatform platform_;
    static std::mutex mutex_;
};
} // namespace pipensx
