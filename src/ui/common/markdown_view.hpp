#pragma once

#include <algorithm>
#include <cmath>
#include <set>
#include <borealis.hpp>
#include <yoga/Yoga.h>
#include "ui/common/markdown_text.hpp"
#include "ui/common/qr_view.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class MarkdownBlockView : public brls::View {
public:
    explicit MarkdownBlockView(markdown::Block block) : block_(std::move(block)) {
        setShrink(0);
        setMarginBottom(block_.kind == markdown::Kind::List ? 6 : 14);
        if (block_.kind == markdown::Kind::Heading) setMarginTop(12);
        YGNodeSetMeasureFunc(getYGNode(), [](YGNodeRef node, float width, YGMeasureMode mode,
                                          float, YGMeasureMode) -> YGSize {
            auto* view = static_cast<MarkdownBlockView*>(YGNodeGetContext(node));
            if (mode == YGMeasureModeUndefined || !std::isfinite(width)) width = 800;
            view->layout(std::max(1.0f, width));
            return {width, view->height_};
        });
    }

    void draw(NVGcontext* vg, float x, float y, float width, float,
              brls::Style, brls::FrameContext*) override {
        layout(width);
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height_);
        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgFontSize(vg, fontSize());
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        const bool code = block_.kind == markdown::Kind::Code;
        if (code || block_.kind == markdown::Kind::Quote || block_.kind == markdown::Kind::Rule) {
            nvgBeginPath(vg);
            if (code) nvgRoundedRect(vg, x, y, width, height_, theme::kRadiusSmall);
            else nvgRect(vg, x, y, block_.kind == markdown::Kind::Rule ? width : 3,
                         block_.kind == markdown::Kind::Rule ? 1 : height_);
            nvgFillColor(vg, a(code ? theme::surface() : theme::textSecondary()));
            nvgFill(vg);
        }
        for (const auto& run : runs_) {
            nvgSave(vg);
            nvgTranslate(vg, x + run.x, y + run.y);
            if (run.style & markdown::Italic) nvgSkewX(vg, -0.12f);
            if ((run.style & markdown::Code) && !code) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, -1, 0, run.width + 2, fontSize() + 3, 2);
                nvgFillColor(vg, a(theme::surface()));
                nvgFill(vg);
            }
            nvgFillColor(vg, a(theme::textPrimary()));
            nvgText(vg, 0, 0, run.text.c_str(), nullptr);
            if ((run.style & markdown::Bold) || block_.kind == markdown::Kind::Heading)
                nvgText(vg, 0.6f, 0, run.text.c_str(), nullptr);
            nvgRestore(vg);
        }
        nvgRestore(vg);
    }

private:
    struct Run { std::string text; int style; float x, y, width; };
    float fontSize() const {
        if (block_.kind == markdown::Kind::Heading)
            return block_.level <= 2 ? theme::kFontHeading : theme::kFontBody;
        return block_.kind == markdown::Kind::Code ? theme::kFontSmall : theme::kFontBody;
    }
    void layout(float width) {
        if (width == width_) return;
        width_ = width;
        runs_.clear();
        if (block_.kind == markdown::Kind::Rule) { height_ = 12; return; }
        auto* vg = brls::Application::getNVGContext();
        nvgSave(vg);
        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgFontSize(vg, fontSize());
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        const bool code = block_.kind == markdown::Kind::Code;
        float inset = code || block_.kind == markdown::Kind::Quote ? 12 : 0;
        float origin = inset;
        if (block_.kind == markdown::Kind::List) {
            const float markerWidth = nvgTextBounds(vg, 0, 0, block_.marker.c_str(), nullptr, nullptr);
            runs_.push_back({block_.marker, 0, 0, 0, markerWidth});
            origin = std::max(28.0f, markerWidth + 10);
        }
        float x = origin, y = code ? 10 : 0;
        const float limit = std::max(origin + 1, width - inset - 2);
        const float lineHeight = fontSize() * 1.3f;
        for (const auto& span : block_.spans) {
            for (size_t at = 0; at < span.text.size();) {
                if (span.text[at] == '\n') { x = origin; y += lineHeight; ++at; continue; }
                const bool space = span.text[at] == ' ' || span.text[at] == '\t';
                size_t end = at + 1;
                if (!space) {
                    while (end < span.text.size() && span.text[end] != ' ' &&
                           span.text[end] != '\t' && span.text[end] != '\n') ++end;
                }
                std::string word = space ? (code && span.text[at] == '\t' ? "    " : " ") : span.text.substr(at, end - at);
                float size = nvgTextBounds(vg, 0, 0, word.c_str(), nullptr, nullptr);
                if (x > origin && x + size > limit) { x = origin; y += lineHeight; }
                if (space && !code && x == origin) { at = end; continue; }
                // Long URLs and file paths must wrap even without word boundaries.
                while (!word.empty()) {
                    size_t take = word.size();
                    while (take > 1 && size > limit - x) {
                        --take;
                        while (take > 0 && (static_cast<unsigned char>(word[take]) & 0xc0) == 0x80) --take;
                        if (take == 0) {
                            take = 1;
                            while (take < word.size() && (static_cast<unsigned char>(word[take]) & 0xc0) == 0x80) ++take;
                            break;
                        }
                        size = nvgTextBounds(vg, 0, 0, word.data(), word.data() + take, nullptr);
                    }
                    const auto part = word.substr(0, take);
                    size = nvgTextBounds(vg, 0, 0, part.c_str(), nullptr, nullptr);
                    runs_.push_back({part, span.style, x, y, size});
                    x += size;
                    word.erase(0, take);
                    if (!word.empty()) { x = origin; y += lineHeight; }
                }
                at = end;
            }
        }
        height_ = y + lineHeight + (code ? 10 : 0);
        nvgRestore(vg);
    }
    markdown::Block block_;
    std::vector<Run> runs_;
    float width_ = -1, height_ = 0;
};

inline brls::Box* markdownView(const std::string& text) {
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setShrink(0);
    std::set<std::string> links;
    for (const auto& block : markdown::parse(text)) {
        box->addView(new MarkdownBlockView(block));
        for (const auto& span : block.spans) {
            if ((span.url.rfind("https://", 0) != 0 && span.url.rfind("http://", 0) != 0) ||
                !links.insert(span.url).second) continue;
            auto* link = new theme::Button();
            link->setText(span.text);
            link->setHeight(48);
            link->setMarginBottom(12);
            link->registerClickAction([url = span.url, title = span.text](brls::View*) {
                auto* content = new brls::Box(brls::Axis::COLUMN);
                content->setPadding(24);
                content->setAlignItems(brls::AlignItems::CENTER);
                auto* qr = new QrCodeView(url, true);
                qr->setWidth(288); qr->setHeight(288);
                content->addView(qr);
                auto* caption = new brls::Label();
                caption->setText(title + "\n" + url);
                caption->setSingleLine(false);
                caption->setMarginTop(16);
                content->addView(caption);
                auto* dialog = new brls::Dialog(content);
                dialog->addButton(tr("pipensx/common/close"), [] {});
                dialog->open();
                return true;
            });
            box->addView(link);
        }
    }
    return box;
}

} // namespace pipensx::ui
