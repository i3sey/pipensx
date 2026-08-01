#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

struct DebridFile {
    std::string id;
    std::string path;
    uint64_t bytes = 0;
};

struct DebridInfo {
    enum class Phase { Creating, AwaitingSelection, Downloading, Ready, Failed };
    Phase phase = Phase::Creating;
    std::string name;
    uint64_t bytes = 0;
    double progress = 0.0;
    std::string rawState;
    std::vector<DebridFile> files;
    std::vector<std::string> links;
};

enum class DebridProviderKind { TorBox, RealDebrid };

class DebridProvider {
public:
    virtual ~DebridProvider() = default;
    virtual bool validate(std::string& error) = 0;
    virtual bool createFromMagnet(const std::string& magnet,
                                  std::string& id, std::string& error) = 0;
    virtual bool createFromFile(const std::string& torrentPath,
                                std::string& id, std::string& error) = 0;
    virtual bool fetchInfo(const std::string& id, DebridInfo& info,
                           std::string& error) = 0;
    virtual bool selectFiles(const std::string& id,
                             const std::vector<std::string>& fileIds,
                             std::string& error) = 0;
    virtual bool resolveDownloadUrl(const std::string& id,
                                    const DebridInfo& info,
                                    size_t kthSelected,
                                    const DebridFile& file,
                                    std::string& url, std::string& error) = 0;
    virtual bool remove(const std::string& id, std::string& error) = 0;
    virtual const char* name() const = 0;
};

} // namespace pipensx
