#pragma once

#include <string>
#include <algorithm>
#include <vector>

namespace pipensx {

inline constexpr char kCatalogStatsUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-games/main/"
    "switch_games_stats.json";

inline constexpr char kCatalogCollectionBaseUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/";

inline constexpr char kCatalogGitLabBaseUrl[] =
    "https://gitlab.com/i3sey/catalog-data/-/raw/main/";
inline constexpr char kCatalogGitVerseBaseUrl[] =
    "https://gitverse.ru/api/repos/i3sey/catalog-data/raw/branch/main/";

inline std::vector<std::string> catalogAuxiliaryUrls(
    const std::string& file, const std::string& sourceUrl) {
    const std::string upstream = file == "switch_games_stats.json"
        ? kCatalogStatsUrl : std::string(kCatalogCollectionBaseUrl) + file;
    std::vector<std::string> urls;
    for (const char* base : {kCatalogGitLabBaseUrl, kCatalogGitVerseBaseUrl})
        if (sourceUrl.rfind(base, 0) == 0)
            urls.push_back(std::string(base) + file);
    urls.push_back(upstream);
    for (const char* base : {kCatalogGitLabBaseUrl, kCatalogGitVerseBaseUrl}) {
        const std::string url = std::string(base) + file;
        if (std::find(urls.begin(), urls.end(), url) == urls.end())
            urls.push_back(url);
    }
    return urls;
}

inline std::string catalogMirrorFile(const std::string& url) {
    for (const char* base : {kCatalogCollectionBaseUrl, kCatalogGitLabBaseUrl,
                            kCatalogGitVerseBaseUrl}) {
        if (url.rfind(base, 0) != 0) continue;
        const std::string file = url.substr(std::string(base).size());
        for (const char* allowed : {"RU_catalog.json", "EN_catalog.json",
                 "ES_catalog.json", "FR_catalog.json", "DE_catalog.json",
                 "IT_catalog.json", "PT_BR_catalog.json", "ZH_Hans_catalog.json",
                 "catalog_diff.json"})
            if (file == allowed) return file;
    }
    return {};
}

inline constexpr char kDefaultCatalogUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/"
    "RU_catalog.json";

inline constexpr char kCatalogDiffUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/"
    "catalog_diff.json";

inline constexpr char kCatalogApiBaseUrl[] =
    "https://api.github.com/repos/Langegen/switch-game-collection/contents/";
inline constexpr char kCatalogMirrorBaseUrl[] =
    "https://gitlab.com/api/v4/projects/88330126/packages/generic/langegen/";
inline constexpr char kCatalogMirrorManifestUrl[] =
    "https://gitlab.com/api/v4/projects/88330126/packages/generic/langegen/"
    "latest/manifest.json";

inline std::string langegenCatalogFile(const std::string& url) {
    if (url.rfind(kCatalogCollectionBaseUrl, 0) != 0)
        return {};
    const std::string file = url.substr(sizeof(kCatalogCollectionBaseUrl) - 1);
    for (const char* allowed : {"RU_catalog.json", "EN_catalog.json",
             "ES_catalog.json", "FR_catalog.json", "DE_catalog.json",
             "IT_catalog.json", "PT_BR_catalog.json", "ZH_Hans_catalog.json",
             "catalog_diff.json"})
        if (file == allowed)
            return file;
    return {};
}

inline std::string langegenCatalogApiUrl(const std::string& file) {
    return std::string(kCatalogApiBaseUrl) + file + "?ref=main";
}

inline constexpr char kLegacyCatalogUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-games/refs/heads/main/"
    "switch_games.json";

// The release channel verifies its catalog against the companion manifest.
inline constexpr char kAlternativeCatalogUrl[] =
    "https://github.com/i3sey/pipensx-catalog/releases/latest/download/"
    "catalog.json";
inline constexpr char kDefaultCatalogManifestUrl[] =
    "https://github.com/i3sey/pipensx-catalog/releases/latest/download/"
    "manifest.json";

inline std::string catalogSourceUrlForLanguage(const std::string& lang) {
    if (lang == "en" || lang == "en-US" || lang == "en-GB")
        return std::string(kCatalogCollectionBaseUrl) + "EN_catalog.json";
    if (lang == "es" || lang == "es-ES" || lang == "es-419")
        return std::string(kCatalogCollectionBaseUrl) + "ES_catalog.json";
    if (lang == "fr" || lang == "fr-FR" || lang == "fr-CA")
        return std::string(kCatalogCollectionBaseUrl) + "FR_catalog.json";
    if (lang == "de" || lang == "de-DE")
        return std::string(kCatalogCollectionBaseUrl) + "DE_catalog.json";
    if (lang == "it" || lang == "it-IT")
        return std::string(kCatalogCollectionBaseUrl) + "IT_catalog.json";
    if (lang == "pt-BR" || lang == "pt" || lang == "pt-PT")
        return std::string(kCatalogCollectionBaseUrl) + "PT_BR_catalog.json";
    if (lang == "zh-CN" || lang == "zh-Hans" || lang == "zh")
        return std::string(kCatalogCollectionBaseUrl) + "ZH_Hans_catalog.json";
    if (lang == "ru" || lang == "ru-RU")
        return std::string(kCatalogCollectionBaseUrl) + "RU_catalog.json";
    return std::string(kCatalogCollectionBaseUrl) + "EN_catalog.json";
}

inline std::string catalogDiffLangKey(const std::string& lang) {
    if (lang == "en" || lang == "en-US" || lang == "en-GB")
        return "en";
    if (lang == "es" || lang == "es-ES" || lang == "es-419")
        return "es";
    if (lang == "fr" || lang == "fr-FR" || lang == "fr-CA")
        return "fr";
    if (lang == "de" || lang == "de-DE")
        return "de";
    if (lang == "it" || lang == "it-IT")
        return "it";
    if (lang == "pt-BR" || lang == "pt" || lang == "pt-PT")
        return "pt_br";
    if (lang == "zh-CN" || lang == "zh-Hans" || lang == "zh")
        return "zh_hans";
    if (lang == "ru" || lang == "ru-RU")
        return "ru";
    return "en";
}

} // namespace pipensx

