#include "ui/settings/bug_report_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
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

// Quiet zone the QR spec asks for around a code, in modules. It is part of the
// card: the paper the code is drawn on extends this far past the last module.
constexpr int kQuietZone = 4;

// Smallest QR ever produced (version 1).
constexpr int kMinModules = 21;

// Gap between neighbouring cards, in pixels.
constexpr float kCardGap = 12.0f;

// Most codes we are willing to put on one screen. Past this the reporter has
// to aim a camera at postage stamps, and every extra code is another chance
// for one to come out unreadable and void the whole report.
constexpr std::size_t kMaxCodes = 6;

qrcodegen::QrCode::Ecc toQrEcc(QrEcc ecc) {
    return ecc == QrEcc::Quartile ? qrcodegen::QrCode::Ecc::QUARTILE
                                  : qrcodegen::QrCode::Ecc::MEDIUM;
}

// Modules a chunk of `bytes` needs at this correction level, or 0 if it does
// not encode at all.
int modulesFor(std::size_t bytes, qrcodegen::QrCode::Ecc ecc) {
    try {
        const std::vector<std::uint8_t> probe(bytes, 0xA5);
        return qrcodegen::QrCode::encodeBinary(probe, ecc).getSize();
    } catch (const std::exception&) {
        return 0;
    }
}

// Largest payload that still fits within `maxModules`, header included, as
// bytes of the compressed stream. Found by trial encodes rather than a
// capacity table: qrcodegen already knows the answer exactly, and a table
// copied out of the spec is one more thing that can silently drift.
std::size_t payloadForModules(int maxModules, qrcodegen::QrCode::Ecc ecc) {
    static std::map<std::pair<int, int>, std::size_t> cache;
    const auto key = std::make_pair(maxModules, static_cast<int>(ecc));
    if (auto found = cache.find(key); found != cache.end())
        return found->second;

    std::size_t lo = 0;                     // known to fit
    std::size_t hi = kBugReportMaxTailBytes; // known not to
    if (modulesFor(kBugReportHeaderSize + 1, ecc) > maxModules) {
        cache[key] = 0;
        return 0;
    }
    while (hi - lo > 1) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int modules = modulesFor(kBugReportHeaderSize + mid, ecc);
        if (modules > 0 && modules <= maxModules)
            lo = mid;
        else
            hi = mid;
    }
    cache[key] = lo;
    return lo;
}

// How the codes are laid out on the free rectangle, and how much log that
// buys. Derived from the box the grid actually got, never hardcoded: the
// screen is what decides how big a module can be, and the module size is what
// decides how much fits.
struct GridPlan {
    std::size_t count = 1;
    std::size_t cols = 1;
    float cell = 0.0f;             // side of one card, pixels
    std::size_t payloadBytes = 0;  // compressed bytes per card
};

// Every arrangement worth trying for this box, best (fewest, largest codes)
// first. Squarish grids only - a 5-across row of tiny codes reads worse than
// two rows of large ones.
std::vector<GridPlan> planGrids(float width, float height, float minPixels,
                                qrcodegen::QrCode::Ecc ecc) {
    std::vector<GridPlan> plans;
    for (std::size_t count = 1; count <= kMaxCodes; ++count) {
        const auto cols = static_cast<std::size_t>(
            std::ceil(std::sqrt(static_cast<double>(count))));
        const std::size_t rows = (count + cols - 1) / cols;
        const float cellW =
            (width - kCardGap * static_cast<float>(cols - 1)) /
            static_cast<float>(cols);
        const float cellH =
            (height - kCardGap * static_cast<float>(rows - 1)) /
            static_cast<float>(rows);
        const float cell = std::floor(std::min(cellW, cellH));
        const int maxModules =
            static_cast<int>(std::floor(cell / minPixels)) - kQuietZone * 2;
        if (maxModules < kMinModules)
            continue;
        const std::size_t payload = payloadForModules(maxModules, ecc);
        if (payload == 0)
            continue;
        plans.push_back({count, cols, cell, payload});
    }
    return plans;
}

// One bug-report chunk drawn as a QR code. Unlike the About page's QrCodeView
// this hardcodes the theme-independent black-on-white "paper"/"ink" tokens
// (theme::qrPaper/qrInk) so a camera photo of a TV - even in dark mode - has
// maximum scan contrast. Encodes raw bytes (encodeBinary), not text.
//
// The card sizes itself to a whole number of module pixels within the cell it
// is given, so the code is as large as the cell allows and the leftover is
// zero instead of a white border of arbitrary width.
//
// The code goes to the GPU as one texture of one texel per module, drawn as a
// single nearest-filtered quad. Drawing a rect per module instead is what the
// About page does and it is fine there for one small code, but deko3d issues a
// draw command per subpath: a screenful of full-size codes is thousands of
// them, which exhausts the frame's command memory and aborts the process
// inside nvgEndFrame.
class ReportQrView : public brls::View {
  public:
    ReportQrView(const std::vector<std::uint8_t>& data,
                 qrcodegen::QrCode::Ecc ecc, float cell) {
        try {
            qr_.emplace(qrcodegen::QrCode::encodeBinary(data, ecc));
        } catch (const std::exception&) {
            qr_.reset();
        }
        cells_ = (qr_ ? qr_->getSize() : kMinModules) + kQuietZone * 2;
        const float pixelsPerModule =
            std::max(1.0f, std::floor(cell / static_cast<float>(cells_)));
        const float size = pixelsPerModule * static_cast<float>(cells_);
        setWidth(size);
        setHeight(size);
        // Upload here rather than lazily in draw(): deko3d uploads a texture by
        // submitting to the render queue and waiting on it, which must not
        // happen while the frame's commands are being recorded. Views are only
        // built outside a frame (see GridBox::onLayout and the Y action).
        texture_ = createTexture(brls::Application::getNVGContext());
    }

    ~ReportQrView() override {
        if (texture_ > 0)
            nvgDeleteImage(brls::Application::getNVGContext(), texture_);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (texture_ <= 0)
            return;
        const NVGpaint paint =
            nvgImagePattern(vg, x, y, width, height, 0.0f, texture_, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

  private:
    // One texel per module, quiet zone included, so the quad scales by a whole
    // number and every module lands on the same count of screen pixels.
    int createTexture(NVGcontext* vg) {
        const NVGcolor paper = theme::qrPaper();
        const NVGcolor ink = theme::qrInk();
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(cells_) * cells_ * 4);
        for (int row = 0; row < cells_; row++) {
            for (int col = 0; col < cells_; col++) {
                const bool dark =
                    qr_ && qr_->getModule(col - kQuietZone, row - kQuietZone);
                const NVGcolor& color = dark ? ink : paper;
                std::uint8_t* texel =
                    &pixels[(static_cast<std::size_t>(row) * cells_ + col) * 4];
                for (int channel = 0; channel < 4; channel++)
                    texel[channel] = static_cast<std::uint8_t>(
                        std::clamp(color.rgba[channel], 0.0f, 1.0f) * 255.0f);
            }
        }
        return nvgCreateImageRGBA(vg, cells_, cells_, NVG_IMAGE_NEAREST,
                                  pixels.data());
    }

    std::optional<qrcodegen::QrCode> qr_;
    int cells_ = kMinModules + kQuietZone * 2;
    int texture_ = 0;
};

// Box that reports its own size once borealis has laid it out. The grid can
// only be built against the rectangle it really got, and that is not known
// until then.
class GridBox : public brls::Box {
  public:
    GridBox() : brls::Box(brls::Axis::COLUMN) {}

    void setResizeHandler(std::function<void(float, float)> handler) {
        handler_ = std::move(handler);
    }

    void onLayout() override {
        brls::Box::onLayout();
        const float width = getWidth();
        const float height = getHeight();
        if (!handler_ || width <= 0.0f || height <= 0.0f)
            return;
        // Rebuilding children re-enters layout; only react to a box that
        // actually changed size, or this never settles.
        if (std::abs(width - laidOutWidth_) < 1.0f &&
            std::abs(height - laidOutHeight_) < 1.0f)
            return;
        laidOutWidth_ = width;
        laidOutHeight_ = height;
        // onLayout arrives from inside yoga's own tree walk, where adding or
        // removing children corrupts the walk. Hand the work to the next main
        // loop tick instead.
        brls::sync([handler = handler_, width, height] {
            handler(width, height);
        });
    }

  private:
    std::function<void(float, float)> handler_;
    float laidOutWidth_ = 0.0f;
    float laidOutHeight_ = 0.0f;
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
                                     std::optional<BugReportFixture> fixture,
                                     bool startDetailed)
    : manager_(manager), catalog_(catalog), metadata_(metadata),
      installed_(installed), fixture_(std::move(fixture)),
      detailed_(startDetailed) {
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(16, 24, 16, 24);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* notice = centeredLabel(theme::kFontCaption, theme::textTertiary());
    notice->setText(tr("pipensx/settings/report_notice"));
    root->addView(notice);

    summary_ = centeredLabel(theme::kFontSmall, theme::textSecondary());
    summary_->setMarginTop(6);
    root->addView(summary_);

    auto* grid = new GridBox();
    grid->setGrow(1);
    grid->setWidthPercentage(100);
    grid->setMarginTop(10);
    grid->setAlignItems(brls::AlignItems::CENTER);
    grid->setJustifyContent(brls::JustifyContent::CENTER);
    // The only focusable view on the screen. Without one, focus stays on the
    // settings row that opened this activity: its highlight hangs over the
    // report, its hints fill the bottom bar, and Y never reaches us. The
    // highlight is hidden because there is nothing here to point at.
    grid->setFocusable(true);
    grid->setHideHighlight(true);
    auto alive = alive_;
    grid->setResizeHandler([this, alive](float width, float height) {
        // The handler runs a tick late (see GridBox::onLayout), by which time
        // the reporter may already have pressed B.
        if (!alive->load())
            return;
        gridWidth_ = width;
        gridHeight_ = height;
        renderReport();
    });
    gridHost_ = grid;
    root->addView(grid);

    caption_ = centeredLabel(theme::kFontBody, theme::textPrimary());
    caption_->setMarginTop(10);
    root->addView(caption_);

    hint_ = centeredLabel(theme::kFontCaption, theme::textTertiary());
    hint_->setMarginTop(4);
    root->addView(hint_);

    frame_ = new brls::AppletFrame(root);
    frame_->setTitle(tr("pipensx/settings/report_bug"));
}

std::string BugReportActivity::renderedState() {
    std::size_t codes = 0;
    if (gridHost_) {
        for (brls::View* row : gridHost_->getChildren())
            codes += static_cast<brls::Box*>(row)->getChildren().size();
    }
    return std::string(detailed_ ? "detailed " : "photo ") +
           std::to_string(codes) + " " +
           (caption_ ? caption_->getFullText() : std::string());
}

BugReportActivity::~BugReportActivity() {
    alive_->store(false);
}

brls::View* BugReportActivity::createContentView() {
    return frame_;
}

void BugReportActivity::onContentAvailable() {
    registerAction(
        tr("pipensx/settings/report_more_detail"), brls::BUTTON_Y,
        [this](brls::View*) {
            detailed_ = !detailed_;
            renderReport();
            return true;
        },
        false);

    captureReport();
    renderReport();
    brls::Application::giveFocus(gridHost_);
}

// Snapshot the device and read the log once, on entry. Re-reading per redraw
// would append another snapshot line every time and shuffle the report id
// under the reporter mid-photo.
void BugReportActivity::captureReport() {
    SystemSnapshot snapshot{};
    if (fixture_) {
        tail_ = fixture_->log;
        snapshot = fixture_->snapshot;
    } else {
        snapshot = writeSystemSnapshot(manager_, catalog_, metadata_,
                                       installed_, "report");
        tail_ = readLogTail(kBugReportMaxTailBytes);
    }

    char freeBytes[16];
    fmt_bytes(freeBytes, sizeof(freeBytes), snapshot.freeBytes);
    summary_->setText(
        tr("pipensx/settings/report_summary", PIPENSX_VERSION,
           std::to_string(HOSVER_MAJOR(snapshot.hos)) + "." +
               std::to_string(HOSVER_MINOR(snapshot.hos)) + "." +
               std::to_string(HOSVER_MICRO(snapshot.hos)),
           freeBytes, static_cast<int>(snapshot.installed),
           static_cast<int>(snapshot.catalog),
           static_cast<int>(snapshot.active),
           static_cast<int>(snapshot.errors)));
    sessionId_ = makeSessionId();
}

void BugReportActivity::renderReport() {
    if (!gridHost_ || gridWidth_ <= 0.0f || gridHeight_ <= 0.0f)
        return;

    // Photographing a TV needs fat modules; a Capture-button screenshot is
    // pixel-exact, so it can afford thin ones and carry far more log.
    const qrcodegen::QrCode::Ecc ecc = detailed_
                                           ? qrcodegen::QrCode::Ecc::MEDIUM
                                           : qrcodegen::QrCode::Ecc::QUARTILE;
    const float minPixels = detailed_ ? 2.0f : 4.0f;

    // Pick the arrangement that loses the least log, and among equals the one
    // with the fewest codes: every extra code is one more the reporter has to
    // get in focus. Losing nothing beats dropping telemetry, which beats
    // cutting history.
    const std::vector<GridPlan> plans =
        planGrids(gridWidth_, gridHeight_, minPixels, ecc);
    BugReport report;
    GridPlan chosen;
    int bestFidelity = -1;
    std::size_t bestCapacity = 0;
    for (const GridPlan& plan : plans) {
        const BugReportConfig config{plan.count, plan.payloadBytes,
                                     detailed_ ? QrEcc::Medium
                                               : QrEcc::Quartile,
                                     detailed_};
        BugReport candidate = buildBugReport(tail_, config, sessionId_);
        const int fidelity =
            (candidate.truncated ? 0 : 2) + (candidate.filtered ? 0 : 1);
        const std::size_t capacity = plan.count * plan.payloadBytes;
        // Truncated candidates are ranked by how much they carried, so a
        // hopeless tail still gets the largest grid rather than the first one.
        const bool better =
            fidelity > bestFidelity ||
            (fidelity == bestFidelity && candidate.truncated &&
             capacity > bestCapacity);
        if (!better)
            continue;
        report = std::move(candidate);
        chosen = plan;
        bestFidelity = fidelity;
        bestCapacity = capacity;
        if (bestFidelity == 3)
            break;
    }
    if (bestFidelity < 0)
        return;

    gridHost_->clearViews();
    const std::size_t count = report.chunks.size();
    for (std::size_t i = 0; i < count;) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::CENTER);
        for (std::size_t c = 0; c < chosen.cols && i < count; ++c, ++i) {
            auto* qr = new ReportQrView(report.chunks[i], ecc, chosen.cell);
            qr->setMargins(kCardGap * 0.5f, kCardGap * 0.5f, kCardGap * 0.5f,
                           kCardGap * 0.5f);
            row->addView(qr);
        }
        gridHost_->addView(row);
    }
    // clearViews() destroyed whatever held focus inside the grid, and the
    // grid itself is what should hold it anyway.
    brls::Application::giveFocus(gridHost_);

    char id[8];
    std::snprintf(id, sizeof(id), "%04X", sessionId_);
    std::string caption;
    if (tail_.empty())
        caption = tr("pipensx/settings/report_empty");
    else if (report.truncated)
        caption = tr("pipensx/settings/report_id_truncated", id,
                     static_cast<int>(count));
    else if (report.filtered)
        caption = tr("pipensx/settings/report_id_filtered", id,
                     static_cast<int>(count));
    else
        caption = tr("pipensx/settings/report_id", id, static_cast<int>(count));
    caption_->setText(caption);

    hint_->setText(detailed_ ? tr("pipensx/settings/report_hint_detailed")
                             : tr("pipensx/settings/report_hint"));
}

}  // namespace pipensx::ui
