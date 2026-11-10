#include "catalog_presentation.hpp"
#include "catalog_section.hpp"
#include "catalog_descriptions.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace pipensx {

namespace {

std::string foldAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

template <typename Less>
bool stableSortCancelled(std::vector<int>& values, Less less,
                         const std::function<bool()>& cancelled) {
    if (values.size() < 2)
        return !cancelled || !cancelled();
    std::vector<int> scratch(values.size());
    size_t checks = 0;
    for (size_t width = 1; width < values.size();) {
        if (cancelled && cancelled())
            return false;
        for (size_t left = 0; left < values.size(); left += width * 2) {
            const size_t middle = std::min(left + width, values.size());
            const size_t right = std::min(left + width * 2, values.size());
            size_t a = left, b = middle, out = left;
            while (a < middle || b < right) {
                if ((++checks & 255u) == 0 && cancelled && cancelled())
                    return false;
                if (b == right ||
                    (a < middle && !less(values[b], values[a])))
                    scratch[out++] = values[a++];
                else
                    scratch[out++] = values[b++];
            }
        }
        values.swap(scratch);
        if (width > values.size() / 2)
            break;
        width *= 2;
    }
    return !cancelled || !cancelled();
}

bool sameBrowseStructure(const CatalogBrowseResult& before,
                         const CatalogBrowseResult& after,
                         const std::function<bool()>& cancelled) {
    if (before.structureHashes.size() != after.structureHashes.size())
        return false;
    for (size_t row = 0; row < after.structureHashes.size(); ++row) {
        if ((row & 255u) == 0 && cancelled && cancelled())
            return false;
        if (before.structureHashes[row] != after.structureHashes[row])
            return false;
    }
    return true;
}

} // namespace

std::vector<std::string> mergeScreenshotUrls(
    const GameMetadata* metadata, const CatalogEntry& entry, size_t limit) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    result.reserve(limit);
    auto append = [&](const std::vector<std::string>& values) {
        for (const std::string& value : values) {
            if (result.size() >= limit)
                return;
            if (!value.empty() && seen.insert(value).second)
                result.push_back(value);
        }
    };
    if (metadata)
        append(metadata->screenshots);
    append(entry.screenshots);
    return result;
}

namespace {

std::string join(const std::vector<std::string>& values) {
    std::ostringstream result;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            result << ", ";
        result << values[i];
    }
    return result.str();
}

bool isHex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool looksLikeTitleId(const std::string& titleId) {
    return titleId.size() == 16 &&
           std::all_of(titleId.begin(), titleId.end(), isHex);
}

} // namespace

CatalogRowPresentation resolveCatalogRow(const CatalogEntry& entry,
                                         const GameMetadata* metadata) {
    CatalogRowPresentation result;
    result.title = metadata && !metadata->name.empty()
        ? metadata->name : entry.title;
    result.titleId = metadata && !metadata->titleId.empty()
        ? metadata->titleId : entry.titleId;
    if (metadata && !metadata->iconUrl.empty()) {
        result.iconUrl = metadata->iconUrl;
        result.iconPreserveAspect = false;
    } else {
        result.iconUrl = entry.posterUrl;
        result.iconPreserveAspect = !result.iconUrl.empty();
    }
    return result;
}

std::string catalogRutrackerTopicUrl(const CatalogEntry& entry) {
    static const std::string prefix =
        "https://rutracker.org/forum/viewtopic.php?t=";
    static const std::string wwwPrefix =
        "https://www.rutracker.org/forum/viewtopic.php?t=";
    const std::string& url = entry.topicUrl;
    size_t offset;
    if (url.compare(0, prefix.size(), prefix) == 0)
        offset = prefix.size();
    else if (url.compare(0, wwwPrefix.size(), wwwPrefix) == 0)
        offset = wwwPrefix.size();
    else
        return {};

    uint64_t id = 0;
    if (offset == url.size() || url.size() - offset > 20)
        return {};
    for (; offset < url.size(); ++offset) {
        const unsigned char c = url[offset];
        if (c < '0' || c > '9' ||
            id > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
            return {};
        id = id * 10 + (c - '0');
    }
    if (id == 0 || (entry.topicId != 0 && entry.topicId != id))
        return {};
    // A bare topicId carries no source provenance, so it cannot identify
    // a RuTracker topic in an arbitrary custom catalog.
    return prefix + std::to_string(id);
}

CatalogPresentation resolveCatalogPresentation(
    const CatalogEntry& entry, const GameMetadata* metadata,
    TextPreference preference, const std::string& locale) {
    CatalogRowPresentation row = resolveCatalogRow(entry, metadata);
    CatalogPresentation result;
    result.title = std::move(row.title);
    result.titleId = std::move(row.titleId);
    result.iconUrl = std::move(row.iconUrl);
    result.iconPreserveAspect = row.iconPreserveAspect;
    if (metadata && !metadata->bannerUrl.empty())
        result.coverUrl = metadata->bannerUrl;
    else
        result.coverUrl = result.iconUrl;
    // CatalogNative still falls back to the metadata prose: 26 catalogue
    // entries carry no description at all, and neither do test fixtures.
    if (preference == TextPreference::CatalogNative &&
        !entry.description.empty())
        result.description = entry.description;
    else if (metadata && !metadata->description.empty())
        result.description = metadata->description;
    else if (metadata && !metadata->intro.empty())
        result.description = metadata->intro;
    else
        result.description = entry.description;
    std::string localized;
    if (offlineCatalogDescription(entry.description, locale, localized))
        result.description = std::move(localized);
    result.developer = entry.developer;
    result.publisher = metadata && !metadata->publisher.empty()
        ? metadata->publisher : entry.publisher;
    // No metadata snapshot has ever carried releaseDate, so this is entry.year
    // in every locale — the Release row is already catalogue-native.
    result.releaseDate = metadata && !metadata->releaseDate.empty()
        ? metadata->releaseDate : entry.year;
    result.genre = metadata && !metadata->categories.empty()
        ? join(metadata->categories) : entry.genre;
    // Structured note wins; legacy free-text scrape stays the fallback for
    // snapshots and custom sources that predate the v2 fields.
    result.performance = !entry.performanceNote.empty() ? entry.performanceNote
                                                       : entry.performance;
    result.multiplayer = entry.multiplayer;
    result.packageType = entry.packageType;
    result.imageFormat = entry.imageFormat;
    result.version = entry.version;
    result.languagesInterface = entry.languagesInterface;
    result.languagesVoice = entry.languagesVoice;
    result.playersMin = entry.playersMin;
    result.playersMax = entry.playersMax;
    result.playersOnline = entry.playersOnline;
    result.topicUrl = catalogRutrackerTopicUrl(entry);
    result.screenshots = mergeScreenshotUrls(metadata, entry, 6);
    return result;
}

bool catalogEntryHasMatchedTitle(const GameMetadata* metadata) {
    return metadata && looksLikeTitleId(metadata->titleId);
}

bool catalogEntryMatchesPlayerFilter(const GameMetadata* metadata,
                                     const CatalogEntry& entry,
                                     PlayerFilter filter) {
    if (filter == PlayerFilter::Any)
        return true;
    if (metadata) {
        switch (filter) {
        case PlayerFilter::Splitscreen:
            return (metadata->modes & kPlayerModeSplit) != 0;
        case PlayerFilter::LocalCoop:
            if (metadata->hasModes)
                return (metadata->modes & kPlayerModeCoop) != 0;
            return metadata->players >= 2;
        case PlayerFilter::Lan:
            return (metadata->modes & kPlayerModeLan) != 0;
        case PlayerFilter::Online:
            return (metadata->modes & kPlayerModeOnline) != 0;
        case PlayerFilter::Any:
            break;
        }
        return true;
    }
    // No metadata match: fall back to the catalogue's own structured
    // players. Only co-op-by-count and online exist there; split-screen
    // and LAN have no catalogue signal.
    switch (filter) {
    case PlayerFilter::LocalCoop:
        return entry.playersMax >= 2;
    case PlayerFilter::Online:
        return entry.playersOnline;
    default:
        return false;
    }
}

std::string catalogFoldForSearch(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            const char folded = (c >= 'A' && c <= 'Z')
                ? static_cast<char>(c + ('a' - 'A')) : text[i];
            out.push_back(folded);
            ++i;
            continue;
        }
        // Cyrillic capitals (two-byte sequences with a 0xD0 lead):
        //   U+0400-U+040F (D0 80-8F, incl. Ё=U+0401) → U+0450-U+045F
        //   U+0410-U+041F (D0 90-9F, А-П) → U+0430-U+043F (same lead)
        //   U+0420-U+042F (D0 A0-AF, Р-Я) → U+0440-U+044F (lead D0→D1)
        // The Р-Я range crosses the D0/D1 UTF-8 boundary, so a plain
        // "+0x20 on the trail byte" would emit invalid bytes.
        if (c == 0xD0 && i + 1 < text.size()) {
            const unsigned char d = static_cast<unsigned char>(text[i + 1]);
            if (d >= 0x80 && d <= 0x8F) {
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(d + 0x10));
                i += 2;
                continue;
            }
            if (d >= 0x90 && d <= 0x9F) {
                out.push_back(static_cast<char>(c));
                out.push_back(static_cast<char>(d + 0x20));
                i += 2;
                continue;
            }
            if (d >= 0xA0 && d <= 0xAF) {
                out.push_back(static_cast<char>(0xD1));
                out.push_back(static_cast<char>(d - 0x20));
                i += 2;
                continue;
            }
        }
        // Any other multi-byte sequence (or truncation): copy the lead byte
        // and let the continuation bytes flow through below.
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool catalogFoldedContains(const std::string& haystack,
                           const std::string& needleFolded) {
    if (needleFolded.empty())
        return true;
    return catalogFoldForSearch(haystack).find(needleFolded) !=
           std::string::npos;
}

namespace {

std::string trimGenreToken(const std::string& token) {
    size_t start = 0;
    while (start < token.size() &&
           (std::isspace(static_cast<unsigned char>(token[start])) ||
            token[start] == '"' || token[start] == '\''))
        ++start;
    size_t end = token.size();
    while (end > start &&
           (std::isspace(static_cast<unsigned char>(token[end - 1])) ||
            token[end - 1] == '"' || token[end - 1] == '\''))
        --end;
    return token.substr(start, end - start);
}

} // namespace

void catalogCanonicalizeGenreToken(const std::string& rawToken,
                                   std::unordered_set<std::string>& out) {
    const std::string token = trimGenreToken(rawToken);
    if (token.empty())
        return;
    const std::string lower = catalogFoldForSearch(token);

    // Multi-genre composites
    if (lower == "action rpg" || lower == "action-rpg" || lower == "action/rpg") {
        out.insert(catalog_genres::kAction);
        out.insert(catalog_genres::kRpg);
        return;
    }
    if (lower == "action-adventure" || lower == "action adventure" ||
        lower == "action adventure > survival" || lower == "action/adventure") {
        out.insert(catalog_genres::kAction);
        out.insert(catalog_genres::kAdventure);
        return;
    }
    if (lower == "strategy arcade") {
        out.insert(catalog_genres::kStrategy);
        out.insert(catalog_genres::kArcade);
        return;
    }
    if (lower == "platformer action" || lower == "stealth action") {
        out.insert(catalog_genres::kPlatformer);
        out.insert(catalog_genres::kAction);
        return;
    }

    // Action
    if (lower == "action" || lower == "actions" || lower == "actionь" ||
        lower == "экшн" || lower == "экшен" || lower == "slasher" ||
        lower == "hack and slash" || lower == "hack-and-slash" ||
        lower == "hack'n'slash" || lower == "hack-n-slash" || lower == "musou" ||
        lower == "stealth" || lower == "stealh") {
        out.insert(catalog_genres::kAction);
        return;
    }

    // Adventure
    if (lower == "adventure" || lower == "adventur" || lower == "aventure" ||
        lower == "advenutre" || lower == "приключение" || lower == "приключения" ||
        lower == "quest") {
        out.insert(catalog_genres::kAdventure);
        return;
    }

    // Arcade
    if (lower == "arcade" || lower == "аркада" || lower == "arkanoid" ||
        lower == "pinball") {
        out.insert(catalog_genres::kArcade);
        return;
    }

    // Platformer
    if (lower == "platformer" || lower == "3d platformer" ||
        lower == "3d platformerm" || lower == "platfromer" ||
        lower == "platfomer" || lower == "platforer" ||
        lower == "платформер" || lower == "runner") {
        out.insert(catalog_genres::kPlatformer);
        return;
    }

    // Puzzle
    if (lower == "puzzle" || lower == "puzze" || lower == "пазл" ||
        lower == "sokoban" || lower == "hidden objects" ||
        lower == "поиск предметов" || lower == "казуальные игры") {
        out.insert(catalog_genres::kPuzzle);
        return;
    }

    // RPG
    if (lower == "role-playing" || lower == "rpg" || lower == "role playing" ||
        lower == "role-play" || lower == "jrpg" || lower == "jrole-playing" ||
        lower == "ролевая игра" || lower == "ролевая" ||
        lower == "dungeon crawler") {
        out.insert(catalog_genres::kRpg);
        return;
    }

    // Strategy
    if (lower == "strategy" || lower == "stategy" || lower == "стратегия" ||
        lower == "стратегии" || lower == "tactic" || lower == "tactics" ||
        lower == "tactical strategy" || lower == "tactical rpg" ||
        lower == "real-time strategy" || lower == "turn based" ||
        lower == "пошаговая" || lower == "tower defense" ||
        lower == "city-building") {
        out.insert(catalog_genres::kStrategy);
        return;
    }

    // Simulation
    if (lower == "simulation" || lower == "simulator" || lower == "симулятор" ||
        lower == "economical simulator" || lower == "farm" || lower == "flight" ||
        lower == "sim" || lower == "sandbox" || lower == "food" ||
        lower == "interactive model kit") {
        out.insert(catalog_genres::kSimulation);
        return;
    }

    // Shooter
    if (lower == "shooter" || lower == "first-person shooter" ||
        lower == "first person shooter" || lower == "fps" ||
        lower == "first-person" || lower == "first person" ||
        lower == "shootemup" || lower == "shoot'em up" || lower == "shoot em up" ||
        lower == "bullet hell" || lower == "twin-stick" || lower == "run & gun" ||
        lower == "run&gun" || lower == "run'n'gun" || lower == "шутер" ||
        lower == "шутер от первого лица") {
        out.insert(catalog_genres::kShooter);
        return;
    }

    // Fighting
    if (lower == "fighting" || lower == "beatemup" || lower == "beat 'em up" ||
        lower == "beatmeup" || lower == "brawler") {
        out.insert(catalog_genres::kFighting);
        return;
    }

    // Racing
    if (lower == "racing" || lower == "racnig" || lower == "гонки" ||
        lower == "bike" || lower == "moto") {
        out.insert(catalog_genres::kRacing);
        return;
    }

    // Sports
    if (lower == "sports" || lower == "sport" || lower == "fitness") {
        out.insert(catalog_genres::kSports);
        return;
    }

    // Board & Card
    if (lower == "board game" || lower == "board games" ||
        lower == "настольная" || lower == "настольная игра" ||
        lower == "tcg" || lower == "tgc" || lower == "карточная пошаговая") {
        out.insert(catalog_genres::kBoardCard);
        return;
    }

    // Party
    if (lower == "party" || lower == "вечеринка") {
        out.insert(catalog_genres::kParty);
        return;
    }

    // Music & Rhythm
    if (lower == "music" || lower == "rhythm" || lower == "musical" ||
        lower == "karaoke" || lower == "музыка" || lower == "ритм-игра" ||
        lower == "audiogame") {
        out.insert(catalog_genres::kMusicRhythm);
        return;
    }

    // Visual Novel
    if (lower == "visual novel" || lower == "visual nove" ||
        lower == "viusal novel" || lower == "interactive fiction" ||
        lower == "interactive fiction (if)" || lower == "if" ||
        lower == "interactive movie" || lower == "fmv" || lower == "otome" ||
        lower == "eroge" || lower == "hentai" || lower == "ecchi") {
        out.insert(catalog_genres::kVisualNovel);
        return;
    }

    // Horror
    if (lower == "horror") {
        out.insert(catalog_genres::kHorror);
        return;
    }

    // Roguelike
    if (lower == "roguelike" || lower == "roguelite" || lower == "rougelite" ||
        lower == "rogulite" || lower == "rogutelite" || lower == "roguelute") {
        out.insert(catalog_genres::kRoguelike);
        return;
    }

    // Metroidvania
    if (lower == "metroidvania" || lower == "metrodivania" ||
        lower == "метроидвания") {
        out.insert(catalog_genres::kMetroidvania);
        return;
    }

    // Point & Click
    if (lower == "point & click" || lower == "point-and-click" ||
        lower == "point and click") {
        out.insert(catalog_genres::kPointClick);
        return;
    }

    // Multiplayer
    if (lower == "multiplayer" || lower == "online multiplayer" ||
        lower == "communication" || lower == "battle royale") {
        out.insert(catalog_genres::kMultiplayer);
        return;
    }

    // Utility / Homebrew / Apps
    if (lower == "homebrew" || lower == "emulation" || lower == "emulator" ||
        lower == "эмулятор для пк" || lower == "application" ||
        lower == "utility" || lower == "education" || lower == "learning" ||
        lower == "study" || lower == "training" || lower == "video" ||
        lower == "lifestyle" || lower == "practical" || lower == "modification") {
        out.insert(catalog_genres::kUtility);
        return;
    }

    // Ignore noise tags (settings/art-styles/placeholders)
    if (lower == "b&w" || lower == "b&w (отдельный режим)" ||
        lower == "weird" || lower == "unknown" || lower == "indie" ||
        lower == "va" || lower == "vr" || lower == "streaming" ||
        lower == "other" || lower == "updates" || lower == "ports") {
        return;
    }

    // Preserve custom / unexpected genre tokens
    out.insert(token);
}

std::vector<std::string> catalogEntryGenres(const CatalogEntry& entry,
                                            const GameMetadata* metadata) {
    std::unordered_set<std::string> canonical;
    if (metadata && !metadata->categories.empty()) {
        for (const std::string& cat : metadata->categories)
            catalogCanonicalizeGenreToken(cat, canonical);
    } else if (!entry.genre.empty()) {
        size_t start = 0;
        for (size_t i = 0; i <= entry.genre.size(); ++i) {
            if (i == entry.genre.size() || entry.genre[i] == ',' ||
                entry.genre[i] == '/' || entry.genre[i] == ';') {
                if (i > start) {
                    catalogCanonicalizeGenreToken(
                        entry.genre.substr(start, i - start), canonical);
                }
                start = i + 1;
            }
        }
    }
    std::vector<std::string> result(canonical.begin(), canonical.end());
    std::sort(result.begin(), result.end());
    return result;
}

bool catalogEntryMatchesGenreFilter(
    const CatalogEntry& entry,
    const GameMetadata* metadata,
    const std::unordered_set<std::string>& filters) {
    if (filters.empty())
        return true;
    const std::vector<std::string> genres = catalogEntryGenres(entry, metadata);
    for (const std::string& g : genres) {
        if (filters.count(g) != 0)
            return true;
    }
    if (metadata) {
        for (const std::string& cat : metadata->categories)
            if (filters.count(cat) != 0)
                return true;
    }
    if (!entry.genre.empty() && filters.count(entry.genre) != 0)
        return true;
    return false;
}

namespace {

using SearchWord = std::u32string;

std::string searchTitle(const std::string& title) {
    std::string result;
    unsigned depth = 0;
    for (char c : title) {
        if (c == '[') { ++depth; result += ' '; }
        else if (c == ']' && depth) --depth;
        else if (!depth) result += c;
    }
    return result;
}

std::vector<SearchWord> searchWords(const std::string& folded) {
    std::vector<SearchWord> words;
    SearchWord word;
    for (size_t i = 0; i < folded.size();) {
        const unsigned char c = folded[i++];
        char32_t cp = c;
        unsigned extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra && i + extra <= folded.size()) {
            cp = c & ((1u << (6 - extra)) - 1);
            while (extra--) cp = (cp << 6) | (folded[i++] & 63);
        }
        if ((cp >= U'a' && cp <= U'z') || (cp >= U'0' && cp <= U'9') ||
            (cp >= U'а' && cp <= U'я') || cp == U'ё') {
            word += cp;
        } else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

int wordDistance(const SearchWord& a, const SearchWord& b, int limit) {
    if (a.size() > 32 || b.size() > 32 ||
        std::abs(static_cast<int>(a.size()) - static_cast<int>(b.size())) > limit)
        return limit + 1;
    std::array<int, 33> prev{}, older{}, next{};
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        next.fill(limit + 1);
        next[0] = static_cast<int>(i);
        int best = limit + 1;
        const size_t start = i > static_cast<size_t>(limit) ? i - limit : 1;
        const size_t end = std::min(b.size(), i + limit);
        for (size_t j = start; j <= end; ++j) {
            next[j] = std::min({prev[j] + 1, next[j - 1] + 1,
                               prev[j - 1] + (a[i - 1] != b[j - 1])});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                next[j] = std::min(next[j], older[j - 2] + 1);
            best = std::min(best, next[j]);
        }
        if (best > limit) return limit + 1;
        older = prev;
        prev = next;
    }
    return prev[b.size()];
}

int titleSearchRank(const std::string& title, const std::string& needle,
                    const std::vector<SearchWord>& query) {
    const std::string folded = catalogFoldForSearch(title);
    if (folded == needle) return 0;
    if (folded.compare(0, needle.size(), needle) == 0) return 10;
    if (folded.find(needle) != std::string::npos) return 20;
    if (query.empty() || query.size() > 8 || needle.size() > 128) return -1;
    // Release tags are searchable literally, but never generate fuzzy aliases.
    const auto words = searchWords(searchTitle(folded));
    if (query.size() == 1 && query[0].size() >= 3 && query[0].size() <= 12) {
        SearchWord initials;
        for (const auto& word : words) initials += word[0];
        if (initials.find(query[0]) != SearchWord::npos) return 25;
        static const std::pair<const char*, const char*> aliases[] = {
            {"botw", "breath of the wild"}, {"totk", "tears of the kingdom"},
            {"smo", "super mario odyssey"}, {"mk8", "mario kart 8"},
            {"mk8d", "mario kart 8 deluxe"}, {"acnh", "animal crossing new horizons"},
            {"ssbu", "super smash bros ultimate"}, {"mhr", "monster hunter rise"},
            {"fe3h", "fire emblem three houses"}, {"xc3", "xenoblade chronicles 3"},
        };
        for (const auto& alias : aliases) {
            if (needle != alias.first) continue;
            const auto target = searchWords(alias.second);
            if (std::search(words.begin(), words.end(), target.begin(), target.end()) != words.end())
                return 25;
            return -1;
        }
    }
    int edits = 0;
    for (const auto& part : query) {
        const int limit = part.size() >= 7 ? 2 : part.size() >= 4 ? 1 : 0;
        int best = limit + 1;
        for (const auto& word : words) {
            if (word.compare(0, part.size(), part) == 0) { best = 0; break; }
            if (limit) best = std::min(best, wordDistance(part, word, limit));
        }
        if (best > limit) return -1;
        edits += best;
    }
    return edits ? 40 + edits : 20;
}

int entrySearchRank(const CatalogEntry& entry, const GameMetadata* metadata,
                    const std::string& needle, const std::vector<SearchWord>& query) {
    int rank = titleSearchRank(entry.title, needle, query);
    if (metadata && !metadata->name.empty()) {
        const int other = titleSearchRank(metadata->name, needle, query);
        if (other >= 0 && (rank < 0 || other < rank)) rank = other;
    }
    if (rank >= 0 && rank <= 20) return rank;
    bool literal = catalogFoldedContains(entry.titleId, needle) ||
                   catalogFoldedContains(entry.genre, needle);
    for (const auto& title : entry.titles)
        literal = literal || catalogFoldedContains(title.titleId, needle);
    if (metadata) {
        for (const auto& category : metadata->categories)
            literal = literal || catalogFoldedContains(category, needle);
    }
    for (const auto& genre : catalogEntryGenres(entry, metadata))
        literal = literal || catalogFoldedContains(genre, needle);
    return literal ? 30 : rank;
}

using ReleaseFacts = std::array<std::string, 6>;

std::string languageFact(const std::vector<std::string>& languages,
                         const std::string& legacy) {
    std::vector<std::string> values;
    for (const auto& lang : languages) {
        if (!lang.empty()) values.push_back(upperAscii(lang));
    }
    if (values.empty()) {
        for (const auto& word : searchWords(catalogFoldForSearch(legacy))) {
            if (word == U"ru" || word == U"rus" || word == U"russian" || word == U"русский")
                values.push_back("RU");
            else if (word == U"en" || word == U"eng" || word == U"english" || word == U"английский")
                values.push_back("EN");
        }
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += "/";
        result += value;
    }
    return result;
}

ReleaseFacts releaseFacts(const CatalogEntry& entry, const CatalogBrowseRequest& request) {
    ReleaseFacts facts;
    facts[0] = upperAscii(entry.imageFormat);
    const auto words = searchWords(catalogFoldForSearch(entry.title));
    if (facts[0].empty()) {
        for (const auto& word : words) {
            if (word == U"nsp") facts[0] = "NSP";
            else if (word == U"nsz") facts[0] = "NSZ";
            else if (word == U"xci") facts[0] = "XCI";
        }
    }
    std::string version = entry.version;
    std::string update = entry.updateVersion;
    if (entry.titles.size() == 1) {
        if (version.empty()) version = entry.titles[0].version;
        if (update.empty()) update = entry.titles[0].updateVersion;
    }
    if (!version.empty()) facts[1] = version[0] == 'v' ? version : "v" + version;
    if ((!update.empty() && update != "0") || entry.packageType == "update")
        facts[2] = request.diffUpdateLabel + (update.empty() ? "" : " " + update);
    const std::string title = catalogFoldForSearch(entry.title);
    bool includesDlc = entry.packageType == "dlc" ||
        title.find("+dlc") != std::string::npos || title.find("+ dlc") != std::string::npos;
    for (size_t i = 1; i < words.size(); ++i) {
        if (words[i] == U"dlc" && words[i - 1][0] >= U'1' && words[i - 1][0] <= U'9' &&
            std::all_of(words[i - 1].begin(), words[i - 1].end(),
                        [](char32_t c) { return c >= U'0' && c <= U'9'; }))
            includesDlc = true;
    }
    if (includesDlc) facts[3] = "+DLC";
    const std::string text = languageFact(entry.languagesInterface, entry.interfaceLang);
    const std::string voice = languageFact(entry.languagesVoice, entry.voiceLang);
    if (!text.empty()) facts[4] = request.diffTextLabel + " " + text;
    if (!voice.empty()) facts[5] = request.diffVoiceLabel + " " + voice;
    return facts;
}

std::string releaseGroup(const CatalogEntry& entry, const GameMetadata* metadata) {
    // A multi-game bundle is not an alternative release of its first game.
    if (entry.titles.size() > 1) return {};
    std::string id = entry.titleId;
    if (id.empty() && entry.titles.size() == 1) id = entry.titles[0].titleId;
    if (id.empty() && metadata) id = metadata->titleId;
    if (!id.empty()) return "id:" + upperAscii(id);
    const auto& name = metadata && !metadata->name.empty() ? metadata->name : entry.title;
    const std::string folded = catalogFoldForSearch(searchTitle(name));
    std::string key;
    for (const auto& word : searchWords(folded)) {
        // Preserve UTF-8 and sequel numbers while ignoring punctuation and tags.
        for (char32_t cp : word) key += std::to_string(static_cast<uint32_t>(cp)) + ",";
        key += ";";
    }
    return key.empty() ? std::string() : "name:" + key;
}

} // namespace

int catalogEntrySearchRank(const CatalogEntry& entry,
                           const GameMetadata* metadata,
                           const std::string& needleFolded) {
    if (needleFolded.empty()) return 0;
    return entrySearchRank(entry, metadata, needleFolded, searchWords(needleFolded));
}

bool catalogEntryMatchesSearch(const CatalogEntry& entry,
                               const GameMetadata* metadata,
                               const std::string& needleFolded) {
    return catalogEntrySearchRank(entry, metadata, needleFolded) >= 0;
}

bool buildCatalogBrowse(const CatalogBrowseRequest& request,
                        CatalogBrowseResult& result,
                        const std::function<bool()>& cancelled) {
    auto isCancelled = [&] { return cancelled && cancelled(); };
    result = {};
    result.generation = request.generation;
    result.catalog = request.catalog
        ? request.catalog
        : std::make_shared<const std::vector<CatalogEntry>>();
    result.metadata = request.metadata
        ? request.metadata
        : std::make_shared<const GameMetadataIndexSnapshot>();
    result.selectedHashes = request.selectedHashes;
    const auto& all = *result.catalog;
    const std::string needle = catalogFoldForSearch(request.query);
    const auto queryWords = searchWords(needle);
    std::vector<int> searchRanks(all.size(), 0);
    result.indices.reserve(all.size());

    std::unordered_set<std::string> genres;
    std::unordered_set<std::string> collectionHashes;
    for (size_t i = 0; i < all.size(); ++i) {
        if ((i & 127u) == 0 && isCancelled())
            return false;
        const CatalogEntry& entry = all[i];
        if (entry.isHiddenByDefault())
            continue;
        result.hasRegularEntries = true;
        const GameMetadata* meta =
            result.metadata->findByInfoHash(entry.infoHash);
        if (catalogEntryInSection(entry, meta, request.section)) {
            const std::vector<std::string> entryGenres =
                catalogEntryGenres(entry, meta);
            for (const std::string& g : entryGenres)
                genres.insert(g);
        }

        const std::string hash = foldAscii(entry.infoHash);
        if (request.favoritesOnly &&
            request.favoriteHashes.count(hash) == 0)
            continue;
        if (request.fitsOnly && request.freeSpaceAvailable && entry.size != 0 &&
            entry.size > request.freeBytes)
            continue;
        if (!catalogEntryInSection(entry, meta, request.section))
            continue;
        if (!catalogEntryMatchesPlayerFilter(meta, entry, request.playerFilter))
            continue;
        if (!request.genreFilters.empty() &&
            !catalogEntryMatchesGenreFilter(entry, meta, request.genreFilters))
            continue;
        searchRanks[i] = needle.empty() ? 0 : entrySearchRank(entry, meta, needle, queryWords);
        if (searchRanks[i] < 0)
            continue;
        if (!request.collection.empty()) {
            if (std::find(entry.collections.begin(), entry.collections.end(),
                          request.collection) == entry.collections.end()) continue;
            if (!collectionHashes.insert(hash).second) continue;
        }
        result.indices.push_back(static_cast<int>(i));
    }
    result.genres.assign(genres.begin(), genres.end());
    std::sort(result.genres.begin(), result.genres.end());
    if (isCancelled())
        return false;

    auto entryAt = [&](int index) -> const CatalogEntry& {
        return all[static_cast<size_t>(index)];
    };
    if (request.sort == CatalogSortMode::Alphabetical) {
        std::unordered_map<int, std::string> keys;
        keys.reserve(result.indices.size());
        size_t keyIndex = 0;
        for (int index : result.indices) {
            if ((keyIndex++ & 127u) == 0 && isCancelled())
                return false;
            keys.emplace(index, catalogFoldForSearch(entryAt(index).title));
        }
        if (!stableSortCancelled(result.indices,
                [&](int a, int b) { return keys[a] < keys[b]; }, cancelled))
            return false;
    } else if (request.sort == CatalogSortMode::Metacritic) {
        if (!stableSortCancelled(result.indices, [&](int a, int b) {
                const auto left = entryAt(a).metacritic;
                const auto right = entryAt(b).metacritic;
                if (left.has_value() != right.has_value())
                    return request.sortReversed ? !left.has_value() : left.has_value();
                return left && right && *left > *right;
            }, cancelled)) return false;
    } else if (request.sort == CatalogSortMode::Largest) {
        if (!stableSortCancelled(result.indices,
                [&](int a, int b) {
                    return entryAt(a).size > entryAt(b).size;
                }, cancelled))
            return false;
    } else if (request.sort == CatalogSortMode::Latest) {
        if (!stableSortCancelled(result.indices,
                [&](int a, int b) {
                    const CatalogEntry& left = entryAt(a);
                    const CatalogEntry& right = entryAt(b);
                    if (left.publishedAt != right.publishedAt)
                        return left.publishedAt > right.publishedAt;
                    if (left.topicId != right.topicId)
                        return left.topicId > right.topicId;
                    return false;
                }, cancelled))
            return false;
    } else {
        bool hasPeers = false;
        for (size_t i = 0; i < result.indices.size(); ++i) {
            if ((i & 127u) == 0 && isCancelled())
                return false;
            hasPeers = hasPeers || entryAt(result.indices[i]).peerCount > 0;
        }
        if (hasPeers) {
            if (!stableSortCancelled(result.indices,
                    [&](int a, int b) {
                        const CatalogEntry& left = entryAt(a);
                        const CatalogEntry& right = entryAt(b);
                        if (left.peerCount != right.peerCount)
                            return left.peerCount > right.peerCount;
                        if (left.publishedAt != right.publishedAt)
                            return left.publishedAt > right.publishedAt;
                        if (left.topicId != right.topicId)
                            return left.topicId > right.topicId;
                        return false;
                    }, cancelled))
                return false;
        } else {
            std::vector<int> ranked = result.indices;
            std::unordered_map<int, size_t> score;
            score.reserve(ranked.size());
            if (!stableSortCancelled(ranked, [&](int a, int b) {
                    const CatalogEntry& left = entryAt(a);
                    const CatalogEntry& right = entryAt(b);
                    if (left.publishedAt != right.publishedAt)
                        return left.publishedAt > right.publishedAt;
                    if (left.topicId != right.topicId)
                        return left.topicId > right.topicId;
                    return false;
                }, cancelled))
                return false;
            for (size_t pos = 0; pos < ranked.size(); ++pos)
                score[ranked[pos]] += pos;
            if (!stableSortCancelled(ranked, [&](int a, int b) {
                    return entryAt(a).size > entryAt(b).size;
                }, cancelled))
                return false;
            for (size_t pos = 0; pos < ranked.size(); ++pos)
                score[ranked[pos]] += pos;
            if (!stableSortCancelled(result.indices, [&](int a, int b) {
                    if (score[a] != score[b])
                        return score[a] < score[b];
                    const CatalogEntry& left = entryAt(a);
                    const CatalogEntry& right = entryAt(b);
                    if (left.publishedAt != right.publishedAt)
                        return left.publishedAt > right.publishedAt;
                    if (left.topicId != right.topicId)
                        return left.topicId > right.topicId;
                    return false;
                }, cancelled))
                return false;
        }
    }
    if (request.sortReversed) {
        for (size_t left = 0, right = result.indices.size();
             left < right && left < --right; ++left) {
            if ((left & 127u) == 0 && isCancelled())
                return false;
            std::swap(result.indices[left], result.indices[right]);
        }
    }
    if (isCancelled())
        return false;

    if (!needle.empty() && !stableSortCancelled(result.indices,
            [&](int a, int b) { return searchRanks[a] < searchRanks[b]; }, cancelled))
        return false;

    const size_t count = result.indices.size();
    result.diffChips.resize(count);
    if (!needle.empty()) {
        std::unordered_map<std::string, std::vector<size_t>> groups;
        std::vector<ReleaseFacts> facts(count);
        for (size_t row = 0; row < count; ++row) {
            if ((row & 127u) == 0 && isCancelled()) return false;
            const auto& entry = entryAt(result.indices[row]);
            const auto key = releaseGroup(entry, result.metadata->findByInfoHash(entry.infoHash));
            if (!key.empty()) groups[key].push_back(row);
            facts[row] = releaseFacts(entry, request);
        }
        for (const auto& group : groups) {
            if (isCancelled()) return false;
            if (group.second.size() < 2) continue;
            for (size_t field = 0; field < ReleaseFacts{}.size(); ++field) {
                const auto& first = facts[group.second.front()][field];
                const bool differs = std::any_of(group.second.begin(), group.second.end(),
                    [&](size_t row) { return facts[row][field] != first; });
                if (!differs) continue;
                for (size_t row : group.second) {
                    if (!facts[row][field].empty()) result.diffChips[row].push_back(facts[row][field]);
                }
            }
        }
    }
    result.titles.reserve(count);
    result.iconUrls.reserve(count);
    result.iconPreserveAspect.reserve(count);
    result.badges.reserve(count);
    result.baseBadges.reserve(count);
    result.favorite.reserve(count);
    result.selected.reserve(count);
    result.selectable.reserve(count);
    result.rowByInfoHash.reserve(count);
    result.rowsByInfoHash.reserve(count);
    result.structureHashes.reserve(count);
    for (size_t rowIndex = 0; rowIndex < count; ++rowIndex) {
        if ((rowIndex & 127u) == 0 && isCancelled())
            return false;
        const CatalogEntry& entry = entryAt(result.indices[rowIndex]);
        const std::string hash = foldAscii(entry.infoHash);
        result.structureHashes.push_back(hash);
        const auto task = request.taskBadges.find(hash);
        const bool selectable = task == request.taskBadges.end();
        if (!selectable)
            result.selectedHashes.erase(hash);
        const GameMetadata* meta =
            result.metadata->findByInfoHash(entry.infoHash);
        CatalogRowPresentation presentation = resolveCatalogRow(entry, meta);
        std::string baseBadge;
        if ((!presentation.titleId.empty() &&
             request.installedTitleIds.count(upperAscii(presentation.titleId)) != 0) ||
            std::any_of(entry.titles.begin(), entry.titles.end(), [&](const CatalogTitle& title) {
                return request.installedTitleIds.count(upperAscii(title.titleId)) != 0;
            }))
            baseBadge = request.installedBadge;
        else if (entry.isAutorun() && entry.autorun) {
            baseBadge = autorunCompatibilityBadge(*entry.autorun);
        }
        std::string badge = selectable ? baseBadge : task->second;
        result.titles.push_back(std::move(presentation.title));
        result.iconUrls.push_back(std::move(presentation.iconUrl));
        result.iconPreserveAspect.push_back(
            presentation.iconPreserveAspect ? 1 : 0);
        result.badges.push_back(std::move(badge));
        result.baseBadges.push_back(std::move(baseBadge));
        result.favorite.push_back(request.favoriteHashes.count(hash) ? 1 : 0);
        result.selected.push_back(
            result.selectedHashes.count(hash) ? 1 : 0);
        result.selectable.push_back(selectable ? 1 : 0);
        result.rowByInfoHash.emplace(hash, rowIndex);
        result.rowsByInfoHash[hash].push_back(rowIndex);
    }
    result.count = count;
    if (request.previous) {
        const bool same = sameBrowseStructure(
            *request.previous, result, cancelled);
        if (isCancelled())
            return false;
        result.structureChanged = !same;
    }
    return !isCancelled();
}

std::vector<size_t> patchCatalogTaskBadge(CatalogBrowseResult& result,
                                          const std::string& infoHash,
                                          bool hasTask,
                                          const std::string& badge) {
    std::vector<size_t> changed;
    const auto found = result.rowsByInfoHash.find(foldAscii(infoHash));
    if (found == result.rowsByInfoHash.end())
        return changed;
    for (size_t row : found->second) {
        if (row >= result.badges.size() || row >= result.baseBadges.size() ||
            row >= result.selectable.size() || row >= result.selected.size())
            continue;
        const std::string& nextBadge = hasTask ? badge : result.baseBadges[row];
        const uint8_t nextSelectable = hasTask ? 0 : 1;
        const bool rowChanged = result.badges[row] != nextBadge ||
            result.selectable[row] != nextSelectable ||
            (hasTask && result.selected[row] != 0);
        result.badges[row] = nextBadge;
        result.selectable[row] = nextSelectable;
        if (hasTask) {
            result.selected[row] = 0;
            result.selectedHashes.erase(foldAscii(infoHash));
        }
        if (rowChanged)
            changed.push_back(row);
    }
    return changed;
}

std::vector<size_t> patchCatalogFavorite(CatalogBrowseResult& result,
                                         const std::string& infoHash,
                                         bool favorite) {
    std::vector<size_t> changed;
    const auto found = result.rowsByInfoHash.find(foldAscii(infoHash));
    if (found == result.rowsByInfoHash.end())
        return changed;
    for (size_t row : found->second) {
        if (row >= result.favorite.size() ||
            result.favorite[row] == static_cast<uint8_t>(favorite))
            continue;
        result.favorite[row] = favorite ? 1 : 0;
        changed.push_back(row);
    }
    return changed;
}

std::vector<size_t> patchCatalogInstalledBadge(
    CatalogBrowseResult& result, const std::string& infoHash,
    const std::unordered_set<std::string>& installedTitleIds,
    const std::string& badge) {
    std::vector<size_t> changed;
    const auto found = result.rowsByInfoHash.find(foldAscii(infoHash));
    if (found == result.rowsByInfoHash.end())
        return changed;
    for (size_t row : found->second) {
        if (row >= result.baseBadges.size() || row >= result.badges.size() ||
            row >= result.selectable.size())
            continue;
        if (row >= result.indices.size())
            continue;
        const int entryIndex = result.indices[row];
        if (entryIndex < 0 || !result.catalog ||
            static_cast<size_t>(entryIndex) >= result.catalog->size())
            continue;
        const CatalogEntry& entry =
            (*result.catalog)[static_cast<size_t>(entryIndex)];
        const GameMetadata* meta = result.metadata
            ? result.metadata->findByInfoHash(entry.infoHash) : nullptr;
        const std::string titleId = resolveCatalogRow(entry, meta).titleId;
        const bool installed = (!titleId.empty() &&
            installedTitleIds.count(upperAscii(titleId)) != 0) ||
            std::any_of(entry.titles.begin(), entry.titles.end(), [&](const CatalogTitle& title) {
                return installedTitleIds.count(upperAscii(title.titleId)) != 0;
            });
        const std::string next = installed ? badge : std::string();
        if (result.baseBadges[row] == next)
            continue;
        const std::string oldBase = result.baseBadges[row];
        result.baseBadges[row] = next;
        if (result.selectable[row] != 0 && result.badges[row] == oldBase) {
            result.badges[row] = next;
            changed.push_back(row);
        }
    }
    return changed;
}

CatalogBrowseGenerationQueue::Ticket
CatalogBrowseGenerationQueue::request() {
    std::lock_guard<std::mutex> lock(mutex_);
    Ticket ticket;
    ticket.generation = ++latest_;
    if (active_ == 0) {
        active_ = ticket.generation;
        ticket.startNow = true;
    } else {
        pending_ = true;
    }
    return ticket;
}

bool CatalogBrowseGenerationQueue::complete(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ != generation)
        return false;
    if (pending_) {
        pending_ = false;
        active_ = latest_;
        return true;
    }
    active_ = 0;
    return false;
}

bool CatalogBrowseGenerationQueue::isCurrent(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation == latest_;
}

size_t CatalogBrowseGenerationQueue::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ ? 1 : 0;
}

size_t CatalogBrowseGenerationQueue::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ == 0 ? 0 : 1;
}

uint64_t CatalogBrowseGenerationQueue::latestGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

CatalogFreshness resolveCatalogFreshness(bool refreshing, uint64_t wallSec,
                                          int64_t snapshotSec, bool hasEntries,
                                          bool isToday) {
    CatalogFreshness out;
    if (refreshing) {
        out.kind = CatalogFreshness::Kind::Updating;
        return out;
    }
    if (wallSec != 0) {
        out.kind = isToday ? CatalogFreshness::Kind::Ok
                           : CatalogFreshness::Kind::Stale;
        out.epochSec = static_cast<int64_t>(wallSec);
        return out;
    }
    if (snapshotSec > 0 && hasEntries) {
        out.kind = isToday ? CatalogFreshness::Kind::Ok
                           : CatalogFreshness::Kind::Stale;
        out.epochSec = snapshotSec;
        return out;
    }
    out.kind = CatalogFreshness::Kind::Never;
    return out;
}

bool catalogAutoRefreshDue(uint64_t wallSec, int64_t cachedSnapshotSec,
                           bool hasCachedEntries, bool wallIsToday,
                           bool cachedIsToday) {
    if (wallSec != 0)
        return !wallIsToday;
    if (hasCachedEntries && cachedSnapshotSec > 0)
        return !cachedIsToday;
    return true;
}

} // namespace pipensx
