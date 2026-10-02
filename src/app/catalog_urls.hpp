#pragma once

// Single source of truth for the built-in pipensx-catalog release channel.
// Header-only so translation units linked without catalog_service.cpp
// (app_settings in unit tests) share the literals without a link dependency.
namespace pipensx {

// Default catalog channel: manifest-verified release asset. An empty
// catalog_source_url means the manifest+catalog pair; a custom URL stays a
// single JSON fetched without any manifest check.
inline constexpr char kDefaultCatalogUrl[] =
    "https://github.com/i3sey/pipensx-catalog/releases/latest/download/"
    "catalog.json";
inline constexpr char kDefaultCatalogManifestUrl[] =
    "https://github.com/i3sey/pipensx-catalog/releases/latest/download/"
    "manifest.json";

} // namespace pipensx
