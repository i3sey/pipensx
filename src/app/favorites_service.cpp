#include "favorites_service.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace pipensx {
namespace {

using Json = nlohmann::json;

// Catalog info hashes are upper-case hex, task ids are lower-case; the rest of
// the app already folds on both sides, so the store keeps one canonical form.
std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::string serialize(const std::vector<FavoriteEntry>& items) {
    Json root = Json::object();
    root["version"] = 1;
    Json list = Json::array();
    for (const FavoriteEntry& item : items) {
        Json entry = Json::object();
        entry["info_hash"] = item.infoHash;
        entry["title"] = item.title;
        list.push_back(std::move(entry));
    }
    root["favorites"] = std::move(list);
    return root.dump(2) + "\n";
}

bool parse(const std::string& text, std::vector<FavoriteEntry>& items,
           std::string& error) {
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "Favourites file is not valid JSON.";
        return false;
    }
    if (!root.contains("favorites"))
        return true;
    if (!root["favorites"].is_array()) {
        error = "Favourites file is malformed: 'favorites' must be a list.";
        return false;
    }
    for (const Json& entry : root["favorites"]) {
        if (!entry.is_object() || !entry.contains("info_hash") ||
            !entry["info_hash"].is_string()) {
            error = "Favourites file is malformed: bad entry.";
            return false;
        }
        FavoriteEntry parsed;
        parsed.infoHash = lowerAscii(entry["info_hash"].get<std::string>());
        if (parsed.infoHash.empty())
            continue;
        if (entry.contains("title") && entry["title"].is_string())
            parsed.title = entry["title"].get<std::string>();
        items.push_back(std::move(parsed));
    }
    return true;
}

} // namespace

FavoritesService::FavoritesService(std::string rootPath)
    : path_(std::move(rootPath) + "/favorites.json") {}

bool FavoritesService::load(std::string& error) {
    items_.clear();
    error.clear();

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        if (errno != ENOENT) {
            error = std::string("Unable to open favourites: ") +
                    std::strerror(errno);
            return false;
        }
        return true;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "Unable to read favourites file.";
        return false;
    }
    std::vector<FavoriteEntry> parsed;
    if (!parse(buffer.str(), parsed, error))
        return false;
    items_ = std::move(parsed);
    return true;
}

bool FavoritesService::contains(const std::string& infoHash) const {
    const std::string needle = lowerAscii(infoHash);
    for (const FavoriteEntry& item : items_)
        if (item.infoHash == needle)
            return true;
    return false;
}

bool FavoritesService::toggle(const std::string& infoHash,
                              const std::string& title, std::string& error) {
    error.clear();
    const std::string key = lowerAscii(infoHash);
    if (key.empty()) {
        error = "Missing info hash.";
        return false;
    }

    std::vector<FavoriteEntry> next = items_;
    auto found = std::find_if(next.begin(), next.end(),
                              [&key](const FavoriteEntry& item) {
                                  return item.infoHash == key;
                              });
    const bool adding = found == next.end();
    if (adding) {
        if (next.size() >= kMaxFavorites) {
            error = "Favourites list is full.";
            return false;
        }
        next.push_back(FavoriteEntry{key, title});
    } else {
        next.erase(found);
    }

    if (!write(next, error))
        return false;
    items_ = std::move(next);
    return adding;
}

bool FavoritesService::write(const std::vector<FavoriteEntry>& items,
                             std::string& error) const {
    std::string temporary = path_ + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create favourites file.";
            return false;
        }
        output << serialize(items);
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write favourites file.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path_.c_str()) == 0)
        return true;

    // Same fallback as AppSettings::write: some FAT drivers refuse to rename
    // over an existing file.
    int renameError = errno;
    if ((unlink(path_.c_str()) == 0 || errno == ENOENT) &&
        rename(temporary.c_str(), path_.c_str()) == 0) {
        return true;
    }
    int finalError = errno;
    unlink(temporary.c_str());
    error = std::string("Unable to replace favourites file: ") +
            std::strerror(finalError ? finalError : renameError);
    return false;
}

} // namespace pipensx
