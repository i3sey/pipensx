#pragma once

#include <string>

namespace pipensx::ui {

// Source descriptions often contain hard-wrapped lines and several empty
// lines between list items. Reflow wrapped text and keep a single paragraph
// boundary for a run containing multiple line breaks (CRLF counts as one).
inline std::string compactDescription(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t breaks = 0;
    bool whitespace = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            whitespace = true;
            if (ch == '\r') {
                ++breaks;
                if (i + 1 < text.size() && text[i + 1] == '\n')
                    ++i;
            } else if (ch == '\n') {
                ++breaks;
            }
            continue;
        }
        if (whitespace && !out.empty())
            out += breaks > 1 ? '\n' : ' ';
        whitespace = false;
        breaks = 0;
        out += ch;
    }
    return out;
}

inline std::string shortDescription(const std::string& text) {
    size_t end = 0;
    size_t characters = 0;
    size_t lines = 1;
    while (end < text.size() && characters < 280) {
        if (text[end] == '\n' && ++lines > 4)
            break;
        ++end;
        while (end < text.size() &&
               (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
            ++end;
        ++characters;
    }
    if (end == text.size())
        return text;
    if (text[end] != ' ' && text[end] != '\n') {
        const size_t word = text.find_last_of(" \n", end - 1);
        if (word != std::string::npos && end - word <= 48)
            end = word;
    }
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\n'))
        --end;
    return text.substr(0, end) + "…";
}

} // namespace pipensx::ui
