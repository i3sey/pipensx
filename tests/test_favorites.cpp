#include "app/favorites_service.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using pipensx::FavoritesService;

namespace {

const char* Root = "/tmp";
const char* Path = "/tmp/favorites.json";

void cleanup() {
    unlink(Path);
    unlink("/tmp/favorites.json.tmp");
}

void writeFile(const std::string& text) {
    std::ofstream output(Path, std::ios::binary | std::ios::trunc);
    output << text;
}

void testMissingFileIsEmptyNotAnError() {
    cleanup();
    FavoritesService favorites(Root);
    std::string error;
    assert(favorites.load(error));
    assert(error.empty());
    assert(favorites.items().empty());
    assert(!favorites.contains("abc"));
}

void testTogglePersistsAndFoldsCase() {
    cleanup();
    FavoritesService favorites(Root);
    std::string error;
    assert(favorites.load(error));

    // Catalog hashes arrive upper-case; task ids elsewhere are lower-case.
    assert(favorites.toggle("AABBCCDD", "Some Game", error));
    assert(error.empty());
    assert(favorites.contains("aabbccdd"));
    assert(favorites.contains("AABBCCDD"));

    FavoritesService reloaded(Root);
    assert(reloaded.load(error));
    assert(reloaded.items().size() == 1);
    assert(reloaded.items()[0].infoHash == "aabbccdd");
    assert(reloaded.items()[0].title == "Some Game");
    assert(reloaded.contains("AaBbCcDd"));
}

void testToggleOffRemoves() {
    cleanup();
    FavoritesService favorites(Root);
    std::string error;
    assert(favorites.load(error));
    assert(favorites.toggle("AABBCCDD", "Some Game", error));
    // Second toggle returns the new state (false = no longer starred) with no
    // error, which is how callers tell removal from a write failure.
    assert(!favorites.toggle("aabbccdd", "Some Game", error));
    assert(error.empty());
    assert(!favorites.contains("aabbccdd"));

    FavoritesService reloaded(Root);
    assert(reloaded.load(error));
    assert(reloaded.items().empty());
}

void testCapRefusesOneTooMany() {
    cleanup();
    FavoritesService favorites(Root);
    std::string error;
    assert(favorites.load(error));
    for (size_t i = 0; i < FavoritesService::kMaxFavorites; ++i) {
        char hash[32];
        std::snprintf(hash, sizeof(hash), "%08zx", i);
        assert(favorites.toggle(hash, "Game", error));
    }
    assert(favorites.items().size() == FavoritesService::kMaxFavorites);

    assert(!favorites.toggle("ffffffff", "One Too Many", error));
    assert(!error.empty());
    assert(favorites.items().size() == FavoritesService::kMaxFavorites);
    assert(!favorites.contains("ffffffff"));

    // The cap blocks adds, never removals.
    assert(!favorites.toggle("00000000", "Game", error));
    assert(error.empty());
    assert(favorites.items().size() == FavoritesService::kMaxFavorites - 1);
}

void testMalformedFileReportsAnError() {
    cleanup();
    writeFile("{not json");
    FavoritesService favorites(Root);
    std::string error;
    assert(!favorites.load(error));
    assert(!error.empty());
    assert(favorites.items().empty());

    writeFile("{\"favorites\": 7}");
    assert(!favorites.load(error));
    assert(!error.empty());

    writeFile("{\"favorites\": [{\"title\": \"no hash\"}]}");
    assert(!favorites.load(error));
    assert(!error.empty());

    // A missing "favorites" key is an empty wishlist, not corruption.
    writeFile("{\"version\": 1}");
    assert(favorites.load(error));
    assert(error.empty());
    assert(favorites.items().empty());
}

} // namespace

int main() {
    testMissingFileIsEmptyNotAnError();
    testTogglePersistsAndFoldsCase();
    testToggleOffRemoves();
    testCapRefusesOneTooMany();
    testMalformedFileReportsAnError();
    cleanup();
    std::puts("favorites tests passed");
    return 0;
}
