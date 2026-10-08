#pragma once

#include "nx_file_types.hpp"

namespace pipensx {

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
        if (stem != "switch_russmod" && stem != "switch_rusmod" &&
            stem != "switch_russian" && stem != "switch_rus" && stem != "switch_ru")
            return {};
    } else if (stem != "switch") return {};
    return parent + "switch";
}

} // namespace pipensx
