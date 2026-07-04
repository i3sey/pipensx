#include "ui/settings/about_view.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "qrcodegen/qrcodegen.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

namespace {

constexpr float kCardGap = theme::kSpacingUnit * 2.0f;
constexpr float kIconFrameSize = 140.0f;
constexpr float kIconSize = 112.0f;
constexpr float kQrSize = 176.0f;

brls::Box* makeCard(brls::Axis axis = brls::Axis::COLUMN) {
    auto* card = new brls::Box(axis);
    card->setBackgroundColor(theme::panel());
    card->setBorderColor(theme::track());
    card->setBorderThickness(1.0f);
    card->setCornerRadius(theme::kRadiusLarge);
    card->setPadding(24, 28, 24, 28);
    return card;
}

brls::Label* addLabel(brls::Box* parent, const std::string& text, float size,
                      NVGcolor color) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setTextColor(color);
    parent->addView(label);
    return label;
}

class QrCodeView : public brls::View {
public:
    explicit QrCodeView(std::string value) : value_(std::move(value)) {
        setWidth(kQrSize);
        setHeight(kQrSize);
        try {
            qr_.emplace(qrcodegen::QrCode::encodeText(
                value_.c_str(), qrcodegen::QrCode::Ecc::MEDIUM));
        } catch (const std::exception&) {
            qr_.reset();
        }
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        const NVGcolor paper =
            brls::Application::getTheme().getColor(
                "brls/button/default_enabled_background");
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, theme::kRadiusMedium);
        nvgFillColor(vg, paper);
        nvgFill(vg);

        if (!qr_)
            return;

        constexpr int kQuietZone = 4;
        const int modules = qr_->getSize();
        const int cells = modules + kQuietZone * 2;
        const float available = std::max(
            0.0f, std::min(width, height) - theme::kSpacingUnit * 2.0f);
        const float cellSize = std::floor(available / static_cast<float>(cells));
        if (cellSize < 1.0f)
            return;

        const float drawnSize = cellSize * static_cast<float>(cells);
        const float originX =
            std::floor(x + (width - drawnSize) * 0.5f);
        const float originY =
            std::floor(y + (height - drawnSize) * 0.5f);

        nvgBeginPath(vg);
        for (int row = 0; row < modules; row++) {
            for (int col = 0; col < modules; col++) {
                if (!qr_->getModule(col, row))
                    continue;
                const float px =
                    originX + static_cast<float>(col + kQuietZone) * cellSize;
                const float py =
                    originY + static_cast<float>(row + kQuietZone) * cellSize;
                nvgRect(vg, px, py, cellSize, cellSize);
            }
        }
        nvgFillColor(vg, theme::textPrimary());
        nvgFill(vg);
    }

private:
    std::string value_;
    std::optional<qrcodegen::QrCode> qr_;
};

brls::Box* makeQrCard(const std::string& title, const std::string& handle,
                      const std::string& summary, const std::string& url) {
    auto* card = makeCard();
    card->setGrow(1);

    auto* titleLabel =
        addLabel(card, title, theme::kFontBody, theme::textPrimary());
    titleLabel->setMarginBottom(6);

    auto* handleLabel =
        addLabel(card, handle, theme::kFontSmall, theme::accent());
    handleLabel->setMarginBottom(6);

    auto* summaryLabel =
        addLabel(card, summary, theme::kFontCaption, theme::textSecondary());
    summaryLabel->setMarginBottom(16);

    auto* qrRow = new brls::Box(brls::Axis::ROW);
    qrRow->setJustifyContent(brls::JustifyContent::CENTER);
    qrRow->setMarginBottom(12);
    qrRow->addView(new QrCodeView(url));
    card->addView(qrRow);

    addLabel(card, url, theme::kFontCaption, theme::textTertiary());
    return card;
}

} // namespace

AboutView::AboutView() : brls::Box(brls::Axis::COLUMN) {
    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(24, 34, 24, 34);

    auto* hero = makeCard(brls::Axis::ROW);
    hero->setAlignItems(brls::AlignItems::CENTER);
    hero->setMarginBottom(kCardGap);

    auto* iconFrame = new brls::Box(brls::Axis::ROW);
    iconFrame->setWidth(kIconFrameSize);
    iconFrame->setHeight(kIconFrameSize);
    iconFrame->setBackgroundColor(theme::surface());
    iconFrame->setCornerRadius(theme::kRadiusLarge);
    iconFrame->setJustifyContent(brls::JustifyContent::CENTER);
    iconFrame->setAlignItems(brls::AlignItems::CENTER);
    iconFrame->setMarginRight(24);
    auto* icon = new brls::Image();
    icon->setWidth(kIconSize);
    icon->setHeight(kIconSize);
    icon->setScalingType(brls::ImageScalingType::FIT);
    icon->setImageFromRes("icon.png");
    iconFrame->addView(icon);
    hero->addView(iconFrame);

    auto* summary = new brls::Box(brls::Axis::COLUMN);
    summary->setGrow(1);
    auto* title =
        addLabel(summary, "pipensx", theme::kFontTitle, theme::textPrimary());
    title->setMarginBottom(10);
    auto* version = addLabel(summary, std::string("Version ") + PIPENSX_VERSION,
                             theme::kFontSmall, theme::accent());
    version->setMarginBottom(4);
    auto* build = addLabel(summary,
                           std::string("Built ") + __DATE__ + " " + __TIME__,
                           theme::kFontCaption, theme::textTertiary());
    build->setMarginBottom(14);
    auto* description = addLabel(
        summary,
        "Nintendo Switch storefront and BitTorrent client for downloading "
        "or streaming NSP/NSZ packages to SD.",
        theme::kFontSmall, theme::textSecondary());
    description->setMarginBottom(12);
    addLabel(summary, "Scan Telegram for support or GitHub for the source tree.",
             theme::kFontCaption, theme::textSecondary());
    hero->addView(summary);
    content->addView(hero);

    auto* qrRow = new brls::Box(brls::Axis::ROW);
    qrRow->setAlignItems(brls::AlignItems::STRETCH);
    qrRow->setMarginBottom(kCardGap);
    auto* telegram = makeQrCard("Telegram", "@i3sey",
                                "Questions, feedback and release updates",
                                "https://t.me/i3sey");
    auto* github = makeQrCard("GitHub", "i3sey/pipensx",
                              "Repository, changelog and issue tracker",
                              "https://github.com/i3sey/pipensx");
    github->setMarginLeft(kCardGap);
    qrRow->addView(telegram);
    qrRow->addView(github);
    content->addView(qrRow);

    auto* details = makeCard();
    auto* detailsTitle =
        addLabel(details, "Project info", theme::kFontBody, theme::textPrimary());
    detailsTitle->setMarginBottom(12);
    auto* storage = addLabel(
        details,
        std::string("Catalog: cached on SD with a bundled offline fallback.\n"
                    "Log: ") + LogPath + "\nSettings: " + SettingsPath,
        theme::kFontSmall, theme::textSecondary());
    storage->setMarginBottom(16);
    auto* libs = addLabel(
        details,
        "Built with libnx, Borealis, libcurl, zstd, mbedTLS and miniupnpc. "
        "See THIRD_PARTY_NOTICES.md for licenses.",
        theme::kFontCaption, theme::textSecondary());
    libs->setMarginBottom(12);
    addLabel(details,
             "pipensx is an independent open-source project and is not "
             "affiliated with Nintendo.",
             theme::kFontCaption, theme::textTertiary());
    content->addView(details);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setContentView(content);
    addView(scroll);
}

} // namespace pipensx::ui
