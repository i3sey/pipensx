#pragma once

#include <cstring>
#include <string>
#include <string_view>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace pipensx {

enum class DirectHint { None, Recommended, Discouraged };

inline bool zonePrefixed(std::string_view zone, std::string_view prefix) {
    if (zone.size() < prefix.size())
        return false;
    if (zone.compare(0, prefix.size(), prefix) != 0)
        return false;
    return zone.size() == prefix.size() || zone[prefix.size()] == '/';
}

inline bool langPrefixed(std::string_view language, std::string_view tag) {
    if (language.size() < tag.size())
        return false;
    if (language.compare(0, tag.size(), tag) != 0)
        return false;
    return language.size() == tag.size() || language[tag.size()] == '-' ||
           language[tag.size()] == '_';
}

inline bool zoneIn(std::string_view zone, const char* const* prefixes,
                   int count) {
    for (int i = 0; i < count; ++i) {
        if (zonePrefixed(zone, prefixes[i]))
            return true;
    }
    return false;
}

inline bool cisTimeZone(std::string_view zone) {
    static const char* const kZones[] = {
        "Europe/Moscow",       "Europe/Kiev",        "Europe/Kyiv",
        "Europe/Uzhgorod",     "Europe/Zaporozhye",  "Europe/Simferopol",
        "Europe/Minsk",        "Europe/Chisinau",    "Europe/Kaliningrad",
        "Europe/Samara",       "Europe/Volgograd",   "Europe/Astrakhan",
        "Europe/Saratov",      "Europe/Ulyanovsk",   "Europe/Kirov",
        "Asia/Yekaterinburg",  "Asia/Omsk",          "Asia/Novosibirsk",
        "Asia/Barnaul",        "Asia/Tomsk",         "Asia/Novokuznetsk",
        "Asia/Krasnoyarsk",    "Asia/Irkutsk",       "Asia/Chita",
        "Asia/Yakutsk",        "Asia/Khandyga",      "Asia/Vladivostok",
        "Asia/Ust-Nera",       "Asia/Magadan",       "Asia/Sakhalin",
        "Asia/Srednekolymsk",  "Asia/Kamchatka",     "Asia/Anadyr",
        "Asia/Almaty",         "Asia/Qyzylorda",     "Asia/Aqtobe",
        "Asia/Aqtau",          "Asia/Atyrau",        "Asia/Oral",
        "Asia/Tashkent",       "Asia/Samarkand",     "Asia/Bishkek",
        "Asia/Dushanbe",       "Asia/Ashgabat",      "Asia/Yerevan",
        "Asia/Baku",           "Asia/Tbilisi",
    };
    return zoneIn(zone, kZones, static_cast<int>(sizeof(kZones) / sizeof(kZones[0])));
}

inline bool strictTimeZone(std::string_view zone) {
    static const char* const kZones[] = {
        "Europe/Berlin", "Europe/Vienna", "Europe/Busingen",
    };
    return zoneIn(zone, kZones, static_cast<int>(sizeof(kZones) / sizeof(kZones[0])));
}

// Time zone wins when it matches either list. Language is only a fallback for
// an unknown or empty zone: ru -> recommended, de -> discouraged.
inline DirectHint classifyDirectHint(std::string_view timeZone,
                                     std::string_view language) {
    if (!timeZone.empty()) {
        if (cisTimeZone(timeZone))
            return DirectHint::Recommended;
        if (strictTimeZone(timeZone))
            return DirectHint::Discouraged;
    }
    if (langPrefixed(language, "ru"))
        return DirectHint::Recommended;
    if (langPrefixed(language, "de"))
        return DirectHint::Discouraged;
    return DirectHint::None;
}

inline std::string consoleTimeZone() {
#ifdef __SWITCH__
    TimeLocationName location{};
    if (R_SUCCEEDED(setsysGetDeviceTimeZoneLocationName(&location)) &&
        location.name[0] != '\0') {
        location.name[sizeof(location.name) - 1] = '\0';
        return location.name;
    }
#endif
    return std::string();
}

inline std::string consoleLanguage() {
#ifdef __SWITCH__
    u64 languageCode = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&languageCode))) {
        const char* name = reinterpret_cast<const char*>(&languageCode);
        char buf[sizeof(languageCode) + 1];
        memcpy(buf, name, sizeof(languageCode));
        buf[sizeof(languageCode)] = '\0';
        return buf;
    }
#endif
    return std::string();
}

}  // namespace pipensx
