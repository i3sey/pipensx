#pragma once

#include "nx_file_types.hpp"

namespace pipensx {

// Recognized language alternative directory or archive component.
inline bool isPortLanguageName(std::string name) {
    for (char& ch : name) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return name == "russian" || name == "rus" || name == "ru" ||
           name == "english" || name == "eng" || name == "en" ||
           name == "german" || name == "de" || name == "deu" ||
           name == "french" || name == "fr" || name == "fra" ||
           name == "spanish" || name == "es" || name == "esp" ||
           name == "italian" || name == "it" || name == "ita" ||
           name == "portuguese" || name == "pt" || name == "pt-br" ||
           name == "japanese" || name == "ja" || name == "jp" ||
           name == "chinese" || name == "zh" || name == "zh-cn" ||
           name == "korean" || name == "ko" || name == "kor" ||
           name == "polish" || name == "pl" || name == "pol";
}

// A named Russian-mod alternative is not a second volume of switch.7z.
// Match only known sibling alternatives; unrelated/complementary archives
// remain selected, and a standalone alternative remains installable.
inline std::string portArchiveChoiceKey(std::string path, bool alternative) {
    if (!isPortArchiveName(path)) return {};
    for (char& ch : path) {
        if (ch == '\\') ch = '/';
        else if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    const size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "" : path.substr(0, slash + 1);
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const std::string stem = name.substr(0, name.find_last_of('.'));
    if (alternative) {
        const bool isAlt =
            stem == "switch_russ" || stem == "switch_rus" || stem == "switch_ru" ||
            stem == "switch_russian" || stem == "switch_russmod" || stem == "switch_rusmod" ||
            stem == "switch_rus_mod" || stem == "switch_russ_mod" ||
            stem.rfind("switch_rus", 0) == 0 || stem.rfind("switch_ru", 0) == 0;
        if (!isAlt)
            return {};
    } else if (stem != "switch") return {};
    return parent + "switch";
}

} // namespace pipensx
