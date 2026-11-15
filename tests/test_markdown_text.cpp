#include "ui/common/markdown_text.hpp"
#include <cassert>
#include <cstdio>

using namespace pipensx::ui::markdown;

static std::string plain(const std::vector<Span>& spans) {
    std::string out;
    for (const auto& span : spans) out += span.text;
    return out;
}

int main() {
    assert(parse("").empty());
    const auto blocks = parse("# Заголовок\r\n\r\nFirst wrapped\nline.\n\n- One\n- **Two**\n12. Three\n> Quote\n---\n```cpp\n*literal*\n  indented\n```\n");
    assert(blocks.size() == 8);
    assert(blocks[0].kind == Kind::Heading && blocks[0].level == 1);
    assert(plain(blocks[0].spans) == "Заголовок");
    assert(plain(blocks[1].spans) == "First wrapped line.");
    assert(blocks[2].kind == Kind::List && blocks[2].marker == "•");
    assert(blocks[3].spans[0].style == Bold);
    assert(blocks[4].marker == "12.");
    assert(blocks[5].kind == Kind::Quote);
    assert(blocks[6].kind == Kind::Rule);
    assert(blocks[7].kind == Kind::Code && plain(blocks[7].spans) == "*literal*\n  indented");
    auto spans = inlineText("**bold** and *italic* and `a_b` and file_name.nro");
    assert(plain(spans) == "bold and italic and a_b and file_name.nro");
    assert(spans[0].style == Bold && spans[2].style == Italic && spans[4].style == Code);
    spans = inlineText("[**Author**](https://example.org/a_(b)) ![Icon](https://example.org/i.png)");
    assert(plain(spans) == "Author Icon");
    assert(spans[0].url == "https://example.org/a_(b)" && spans[0].style == Bold);
    assert(spans.back().url.empty());
    assert(plain(inlineText("[Português abaixo](#português)")) == "Português abaixo");
    assert(plain(inlineText("\\*literal\\* **unclosed [bad](")) == "*literal* **unclosed [bad](");
    assert(plain(inlineText("``a ` b``")) == "a ` b");
    assert(plain(inlineText("**bold *italic* text**")) == "bold italic text");
    assert(inlineText("**bold *italic* text**")[1].style == (Bold | Italic));
    assert(inlineText("***both***")[0].style == (Bold | Italic));
    assert(plain(parse("~~~\n<script>\n")[0].spans) == "<script>");
    assert(plain(parse("#hashtag\nfile_name.nro")[0].spans) == "#hashtag file_name.nro");
    std::puts("markdown text tests passed");
}
