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

} // namespace pipensx::ui
