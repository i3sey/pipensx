#pragma once

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace pipensx::ui::markdown {

enum class Kind { Paragraph, Heading, List, Quote, Code, Rule };
enum Style { Plain = 0, Bold = 1, Italic = 2, Code = 4 };
struct Span {
    std::string text;
    int style = Plain;
    std::string url;
};
struct Block {
    Kind kind = Kind::Paragraph;
    int level = 0;
    std::string marker;
    std::vector<Span> spans;
};

inline void append(std::vector<Span>& out, std::string text, int style, const std::string& url = {}) {
    if (text.empty()) return;
    if (!out.empty() && out.back().style == style && out.back().url == url)
        out.back().text += text;
    else out.push_back({std::move(text), style, url});
}

inline std::vector<Span> inlineText(const std::string& text, int style = Plain, unsigned depth = 0) {
    std::vector<Span> out;
    if (depth > 8) { append(out, text, style); return out; }
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\\' && i + 1 < text.size() && std::ispunct(static_cast<unsigned char>(text[i + 1]))) {
            append(out, text.substr(i + 1, 1), style); i += 2; continue;
        }
        if (text[i] == '`') {
            size_t count = 1;
            while (i + count < text.size() && text[i + count] == '`') ++count;
            const auto end = text.find(std::string(count, '`'), i + count);
            if (end != std::string::npos) {
                append(out, text.substr(i + count, end - i - count), style | Code);
                i = end + count; continue;
            }
        }
        const bool image = text[i] == '!' && i + 1 < text.size() && text[i + 1] == '[';
        if (text[i] == '[' || image) {
            const size_t begin = i + (image ? 2 : 1);
            const size_t close = text.find("](", begin);
            if (close != std::string::npos) {
                size_t end = close + 2;
                int nesting = 1;
                for (; end < text.size(); ++end) {
                    if (text[end] == '(') ++nesting;
                    if (text[end] == ')' && --nesting == 0) break;
                }
                if (end < text.size()) {
                    auto spans = inlineText(text.substr(begin, close - begin), style, depth + 1);
                    for (auto& span : spans) {
                        span.url = image ? "" : text.substr(close + 2, end - close - 2);
                        append(out, span.text, span.style, span.url);
                    }
                    i = end + 1; continue;
                }
            }
        }
        if (text[i] == '*' || text[i] == '_') {
            const char ch = text[i];
            const bool insideWord = ch == '_' && i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1]));
            size_t count = i + 1 < text.size() && text[i + 1] == ch ? 2 : 1;
            if (count == 2 && i + 2 < text.size() && text[i + 2] == ch) count = 3;
            const auto end = text.find(std::string(count, ch), i + count);
            if (!insideWord && end != std::string::npos && end > i + count &&
                !std::isspace(static_cast<unsigned char>(text[i + count])) &&
                !std::isspace(static_cast<unsigned char>(text[end - 1]))) {
                for (const auto& span : inlineText(text.substr(i + count, end - i - count),
                        style | (count == 3 ? Bold | Italic : count == 2 ? Bold : Italic), depth + 1))
                    append(out, span.text, span.style, span.url);
                i = end + count; continue;
            }
        }
        append(out, text.substr(i++, 1), style);
    }
    return out;
}

inline std::string trim(std::string value) {
    const auto start = value.find_first_not_of(" \t\r");
    if (start == std::string::npos) return {};
    return value.substr(start, value.find_last_not_of(" \t\r") - start + 1);
}

inline std::vector<Block> parse(const std::string& text) {
    std::vector<Block> out;
    std::istringstream input(text);
    std::string line, paragraph, code, fence;
    auto flush = [&] {
        if (!paragraph.empty()) out.push_back({Kind::Paragraph, 0, {}, inlineText(paragraph)});
        paragraph.clear();
    };
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto clean = trim(line);
        if (!fence.empty()) {
            if (clean.size() >= fence.size() && clean.find_first_not_of(fence[0]) == std::string::npos) {
                out.push_back({Kind::Code, 0, {}, {{code, Code, {}}}});
                fence.clear(); code.clear();
            } else {
                if (!code.empty()) code += '\n';
                code += line;
            }
            continue;
        }
        if (clean.compare(0, 3, "```") == 0 || clean.compare(0, 3, "~~~") == 0) {
            flush();
            fence = clean.substr(0, clean.find_first_not_of(clean[0]));
            continue;
        }
        if (clean.empty()) { flush(); continue; }
        const bool rule = clean.size() >= 3 && (clean[0] == '-' || clean[0] == '*' || clean[0] == '_') &&
            clean.find_first_not_of(std::string(1, clean[0]) + " \t") == std::string::npos;
        if (rule) { flush(); out.push_back({Kind::Rule, 0, {}, {}}); continue; }
        size_t hashes = clean.find_first_not_of('#');
        if (hashes > 0 && hashes <= 6 && hashes < clean.size() && clean[hashes] == ' ') {
            flush(); out.push_back({Kind::Heading, static_cast<int>(hashes), {}, inlineText(trim(clean.substr(hashes)))}); continue;
        }
        if (clean[0] == '>') {
            flush(); out.push_back({Kind::Quote, 0, {}, inlineText(trim(clean.substr(1)))}); continue;
        }
        size_t prefix = 0;
        std::string marker;
        if (clean.size() > 2 && (clean[0] == '-' || clean[0] == '+' || clean[0] == '*') && clean[1] == ' ') {
            prefix = 2; marker = "•";
        } else {
            const auto digits = clean.find_first_not_of("0123456789");
            if (digits > 0 && digits < clean.size() && digits + 1 < clean.size() &&
                (clean[digits] == '.' || clean[digits] == ')') && clean[digits + 1] == ' ') {
                prefix = digits + 2; marker = clean.substr(0, digits) + ".";
            }
        }
        if (prefix) {
            flush(); out.push_back({Kind::List, 0, marker, inlineText(clean.substr(prefix))}); continue;
        }
        if (!paragraph.empty()) paragraph += ' ';
        paragraph += clean;
    }
    flush();
    if (!fence.empty()) out.push_back({Kind::Code, 0, {}, {{code, Code, {}}}});
    return out;
}

} // namespace pipensx::ui::markdown
