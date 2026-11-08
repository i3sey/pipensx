#pragma once

#include <string>

namespace pipensx {

inline constexpr char kCatalogCollectionBaseUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/";

inline constexpr char kDefaultCatalogUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/"
    "RU_catalog.json";

inline constexpr char kCatalogDiffUrl[] =
    "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/"
    "catalog_diff.json";

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

