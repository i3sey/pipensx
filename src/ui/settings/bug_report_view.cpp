#include "ui/settings/bug_report_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "app/bug_report.hpp"
#include "qrcodegen/qrcodegen.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

extern "C" {
#include "core/util.h"
}

namespace pipensx::ui {

namespace {

qrcodegen::QrCode::Ecc toQrEcc(QrEcc ecc) {
    return ecc == QrEcc::Quartile ? qrcodegen::QrCode::Ecc::QUARTILE
                                  : qrcodegen::QrCode::Ecc::MEDIUM;
}

// One bug-report chunk drawn as a QR code. Unlike the About page's QrCodeView
// this hardcodes the theme-independent black-on-white "paper"/"ink" tokens
// (theme::qrPaper/qrInk) so a camera photo of a TV — even in dark mode — has
// maximum scan contrast. Encodes raw bytes (encodeBinary), not text.
class ReportQrView : public brls::View {
  public:
    ReportQrView(const std::vector<std::uint8_t>& data,
                 qrcodegen::QrCode::Ecc ecc, float size) {
        setWidth(size);
        setHeight(size);
        try {
            qr_.emplace(qrcodegen::QrCode::encodeBinary(data, ecc));
        } catch (const std::exception&) {
            qr_.reset();
        }
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, theme::kRadiusSmall);
        nvgFillColor(vg, theme::qrPaper());
        nvgFill(vg);

        if (!qr_)
            return;

        constexpr int kQuietZone = 4;
        const int modules = qr_->getSize();
        const int cells = modules + kQuietZone * 2;
        const float available =
            std::max(0.0f, std::min(width, height) - theme::kSpacingUnit * 2.0f);
        const float cellSize = std::floor(available / static_cast<float>(cells));
        if (cellSize < 1.0f)
            return;

        const float drawnSize = cellSize * static_cast<float>(cells);
        const float originX = std::floor(x + (width - drawnSize) * 0.5f);
        const float originY = std::floor(y + (height - drawnSize) * 0.5f);

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
        nvgFillColor(vg, theme::qrInk());
        nvgFill(vg);
    }

  private:
    std::optional<qrcodegen::QrCode> qr_;
};

// Read the last `maxBytes` of the log. The bug report only wants recent
// history, and the encoder trims further to fit the grid regardless. This goes
// through log_read_tail (the one open handle) rather than fopen: the Switch
// hands out no second handle on a file this process already holds open.
std::string readLogTail(std::size_t maxBytes) {
    std::string out(maxBytes, '\0');
    out.resize(log_read_tail(&out[0], maxBytes));
    return out;
}

std::uint16_t makeSessionId() {
#ifdef __SWITCH__
    std::uint8_t bytes[2];
    rand_bytes(bytes, sizeof(bytes));
    return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
#else
    // Deterministic on PC so the golden screenshot of this screen is stable.
    return 0x5A5A;
#endif
}

brls::Label* centeredLabel(float fontSize, NVGcolor color) {
    auto* label = new brls::Label();
    label->setFontSize(fontSize);
    label->setTextColor(color);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    label->setWidthPercentage(100);
    return label;
}

}  // namespace

BugReportActivity::BugReportActivity(DownloadManager* manager,
                                     CatalogService* catalog,
                                     GameMetadataService* metadata,
                                     InstalledTitleService* installed,
                                     std::optional<std::string> logOverride,
                                     bool startDetailed)
    : manager_(manager), catalog_(catalog), metadata_(metadata),
      installed_(installed), logOverride_(std::move(logOverride)),
      detailed_(startDetailed) {
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(20, 24, 20, 24);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* notice = centeredLabel(theme::kFontSmall, theme::textSecondary());
    notice->setText(tr("pipensx/settings/report_notice"));
    notice->setMarginBottom(10);
    root->addView(notice);

    gridHost_ = new brls::Box(brls::Axis::COLUMN);
    gridHost_->setGrow(1);
    gridHost_->setAlignItems(brls::AlignItems::CENTER);
    gridHost_->setJustifyContent(brls::JustifyContent::CENTER);
    root->addView(gridHost_);

    caption_ = centeredLabel(theme::kFontBody, theme::textPrimary());
    caption_->setMarginTop(10);
    root->addView(caption_);

    hint_ = centeredLabel(theme::kFontCaption, theme::textTertiary());
    hint_->setMarginTop(4);
    root->addView(hint_);

    frame_ = new brls::AppletFrame(root);
    frame_->setTitle(tr("pipensx/settings/report_bug"));
}

brls::View* BugReportActivity::createContentView() {
    return frame_;
}

void BugReportActivity::onContentAvailable() {
    registerAction(
        tr("pipensx/settings/report_more_detail"), brls::BUTTON_Y,
        [this](brls::View*) {
            detailed_ = !detailed_;
            rebuild();
            return true;
        },
        false);
    rebuild();
}

void BugReportActivity::rebuild() {
    std::string tail;
    if (logOverride_) {
        tail = *logOverride_;
    } else {
        // Snapshot device state into the log, then flush both writers (the C
        // core and the borealis handle share the file) before reading it back.
        writeSystemSnapshot(manager_, catalog_, metadata_, installed_, "report");
        tail = readLogTail(128 * 1024);
    }

    const std::uint16_t sessionId = makeSessionId();
    const BugReportConfig& config =
        detailed_ ? kBugReportDetailed : kBugReportDefault;
    BugReport report = buildBugReport(tail, config, sessionId);

    gridHost_->clearViews();
    const qrcodegen::QrCode::Ecc ecc = toQrEcc(config.ecc);
    const float cell = detailed_ ? 150.0f : 210.0f;
    const std::size_t count = report.chunks.size();
    const std::size_t cols = std::max<std::size_t>(
        1, static_cast<std::size_t>(
               std::ceil(std::sqrt(static_cast<double>(count)))));

    for (std::size_t i = 0; i < count;) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::CENTER);
        for (std::size_t c = 0; c < cols && i < count; ++c, ++i) {
            auto* qr = new ReportQrView(report.chunks[i], ecc, cell);
            qr->setMargins(6, 6, 6, 6);
            row->addView(qr);
        }
        gridHost_->addView(row);
    }

    char id[8];
    std::snprintf(id, sizeof(id), "%04X", sessionId);
    std::string caption;
    if (tail.empty())
        caption = tr("pipensx/settings/report_empty");
    else if (report.truncated)
        caption = tr("pipensx/settings/report_id_truncated", id,
                     static_cast<int>(count));
    else
        caption = tr("pipensx/settings/report_id", id, static_cast<int>(count));
    caption_->setText(caption);

    hint_->setText(detailed_ ? tr("pipensx/settings/report_hint_detailed")
                             : tr("pipensx/settings/report_hint"));
}

}  // namespace pipensx::ui
