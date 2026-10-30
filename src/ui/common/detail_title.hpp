#pragma once

#include <algorithm>
#include <string>

namespace pipensx::ui {

// Keep release packaging/language tags in the release details, not the header.
// Match known technical tags only: brackets can also be part of a game title.
inline std::string detailPageTitle(const std::string& title) {
    std::string folded = title;
    for (char& c : folded)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    size_t end = title.size();
    for (const char* tag : {"[repack]", "[nsz]", "[nsp]", "[xci]",
                            "[rus", "[eng", "[multi"}) {
        size_t at = folded.find(tag);
        // Language tags must end or continue with a language separator.
        if (at != std::string::npos && tag[std::char_traits<char>::length(tag) - 1] != ']') {
            const size_t next = at + std::char_traits<char>::length(tag);
            if (next == folded.size() ||
                std::string("]/,+ ").find(folded[next]) == std::string::npos)
                continue;
        }
        end = std::min(end, at);
    }
    std::string out = title.substr(0, end);
    for (const char* note : {"(со сжатым видео)", "(compressed video)"}) {
        const size_t at = out.find(note);
        if (at != std::string::npos)
            out.erase(at, std::char_traits<char>::length(note));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? title : out;
}

} // namespace pipensx::ui
