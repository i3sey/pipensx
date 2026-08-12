#pragma once

#include <string>
#include <utility>

#include <borealis.hpp>

#include "app/catalog_presentation.hpp"

namespace pipensx::ui {

// Translated UI string. Keys live in resources/i18n/<locale>/pipensx.json and
// are addressed as "pipensx/<area>/<name>" — borealis maps a locale file's
// basename to the first path segment.
//
// A free function rather than the brls `_i18n` literal: no pipensx file pulls
// in `using namespace brls`, and the literal would need that using-declaration
// injected into every UI header.
//
// Every string is routed through fmt::format(fmt::runtime(...)), so a format
// argument count that disagrees with the translation throws at runtime rather
// than at compile time. scripts/check_i18n.py holds the placeholder counts of
// every locale to the en-US source, and runs as part of `make test`.
template <typename... Args>
inline std::string tr(const char* key, Args&&... args) {
    return brls::getStr(key, std::forward<Args>(args)...);
}

// The metadata index is English and the Langegen catalogue is Russian, so a
// Russian UI reads better from the catalogue's own prose. Matches "ru" and any
// future regional variant of it.
inline bool preferCatalogNativeText() {
    return brls::Application::getLocale().rfind("ru", 0) == 0;
}

inline TextPreference catalogTextPreference() {
    return preferCatalogNativeText() ? TextPreference::CatalogNative
                                     : TextPreference::Metadata;
}

}  // namespace pipensx::ui
