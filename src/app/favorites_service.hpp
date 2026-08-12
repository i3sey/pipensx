#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pipensx {

// One starred catalog entry. The title rides along with the hash so a favourite
// that disappears from a later catalog refresh can still be named rather than
// showing up as a bare hash.
struct FavoriteEntry {
    std::string infoHash;  // lower-case hex, folded on the way in
    std::string title;
};

// Wishlist store, persisted next to the other app state as favorites.json.
//
// Deliberately not part of AppSettingsData: Settings has a Reset action wired
// to AppSettings::reset(), which assigns a default-constructed AppSettingsData
// — folding the wishlist in there would let a settings reset silently wipe it.
class FavoritesService {
public:
    // An unbounded file on SD with no UI to prune it is the failure mode, so
    // adds stop here.
    static constexpr size_t kMaxFavorites = 500;

    explicit FavoritesService(std::string rootPath);

    // Missing file is an empty wishlist, not an error.
    bool load(std::string& error);

    bool contains(const std::string& infoHash) const;

    // Flips the starred state of infoHash and persists. Returns the new state;
    // on a write failure or when the cap is reached it returns false with a
    // non-empty error and leaves the in-memory list untouched.
    bool toggle(const std::string& infoHash, const std::string& title,
                std::string& error);

    const std::vector<FavoriteEntry>& items() const { return items_; }
    const std::string& path() const { return path_; }

private:
    bool write(const std::vector<FavoriteEntry>& items,
               std::string& error) const;

    std::string path_;
    std::vector<FavoriteEntry> items_;
};

} // namespace pipensx
