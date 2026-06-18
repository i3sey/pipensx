#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pipensx {

struct GameMetadata {
    std::string infoHash;
    std::string titleId;
    std::string match;
    std::string name;
    std::string intro;
    std::string description;
    std::string publisher;
    std::string releaseDate;
    std::string iconUrl;
    std::string bannerUrl;
    std::vector<std::string> screenshots;
    std::vector<std::string> categories;
};

class GameMetadataService {
public:
    explicit GameMetadataService(std::string rootPath,
                                 std::string bundledPath =
                                     "romfs:/catalog/game_metadata_index.json");

    bool load(std::string& error);
    const GameMetadata* findByInfoHash(const std::string& infoHash) const;
    bool refreshDetails(const std::string& titleId, GameMetadata& metadata,
                        std::string& error) const;
    bool loadImage(const std::string& url, std::vector<uint8_t>& bytes,
                   std::string& error) const;

    size_t size() const { return byHash_.size(); }

    static bool parseIndex(const std::string& json,
                           std::vector<GameMetadata>& items,
                           std::string& error);

private:
    std::string rootPath_;
    std::string cacheRoot_;
    std::string imageRoot_;
    std::string bundledPath_;
    std::unordered_map<std::string, GameMetadata> byHash_;
};

} // namespace pipensx
