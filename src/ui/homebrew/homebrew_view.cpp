#include "homebrew_view.hpp"

#include "app/catalog_urls.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/detail_frame.hpp"
#include "ui/common/markdown_view.hpp"
#include "ui/common/stick_scrolling_frame.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"
#include <algorithm>

namespace pipensx::ui {
namespace {
brls::Label* label(brls::Box* box, const std::string& text, float font = theme::kFontSmall) {
    auto* result = new brls::Label();
    result->setText(text);
    result->setFontSize(font);
    result->setTextColor(theme::textPrimary());
    result->setSingleLine(false);
    result->setMarginBottom(12);
    box->addView(result);
    return result;
}

theme::Button* button(brls::Box* box, const std::string& text, std::function<void()> action) {
    auto* result = new theme::Button();
    result->setText(text);
    result->setHeight(48);
    result->setMarginRight(12);
    result->registerClickAction([action = std::move(action)](brls::View*) { action(); return true; });
    box->addView(result);
    return result;
}

struct InstallJob {
    std::atomic<bool> alive{true}, cancelled{false};
    std::atomic<uint64_t> received{0}, total{0};
};

class HomebrewDetail : public brls::Box {
public:
    HomebrewDetail(HomebrewPackage package, std::shared_ptr<HomebrewCatalog> service,
                   GameMetadataService* metadata,
                   std::function<void(const HomebrewPackage&)> onInstalled)
        : brls::Box(brls::Axis::COLUMN), package_(std::move(package)), service_(std::move(service)),
          job_(std::make_shared<InstallJob>()), onInstalled_(std::move(onInstalled)) {
        setGrow(1);
        setAxis(brls::Axis::ROW);
        setPadding(24, 40, 24, 40);
        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidth(320);
        left->setShrink(0);
        left->setMarginRight(32);
        auto* icon = new AsyncRgbaImage();
        icon->setWidth(320); icon->setHeight(320);
        icon->setShrink(0);
        icon->setCornerRadius(theme::kRadiusSmall);
        icon->setScalingType(brls::ImageScalingType::FIT);
        icon->setClipsToBounds(false);
        loadImageInto(icon, metadata, package_.iconUrl);
        left->addView(icon);
        install_ = button(left, tr(package_.installedVersion.empty()
            ? "pipensx/homebrew/install" : "pipensx/homebrew/reinstall"), [this] { confirm(); });
        install_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        install_->setHeight(64);
        install_->setShrink(0);
        install_->setMarginTop(16);
        install_->setMarginRight(0);
        cancel_ = button(left, tr("pipensx/common/cancel"), [this] {
            job_->cancelled = true;
            status_->setText(tr("pipensx/homebrew/cancelling"));
        });
        cancel_->setMarginTop(12);
        cancel_->setMarginRight(0);
        cancel_->setVisibility(brls::Visibility::GONE);
        status_ = label(left, package_.installedVersion.empty()
            ? ""
            : tr("pipensx/homebrew/installed_version", package_.installedVersion));
        status_->setMarginTop(16);
        status_->setTextColor(theme::textSecondary());
        if (package_.installedVersion.empty()) status_->setVisibility(brls::Visibility::GONE);
        addView(left);

        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(0, 32, 24, 8);
        label(content, package_.title, theme::kFontTitle)->setMarginBottom(16);
        label(content, package_.author + " · " + package_.version, theme::kFontBody)
            ->setTextColor(theme::textSecondary());
        if (!package_.description.empty()) {
            auto* description = markdownView(package_.description);
            description->setMarginTop(16);
            content->addView(description);
        }
        if (package_.systemModule) label(content, tr("pipensx/homebrew/system_module"));
        if (!package_.dependencies.empty()) {
            std::string dependencies;
            for (const auto& dependency : package_.dependencies) {
                if (!dependencies.empty()) dependencies += ", ";
                dependencies += dependency;
            }
            label(content, tr("pipensx/homebrew/dependencies", dependencies));
        }
        label(content, tr("pipensx/homebrew/files"), theme::kFontBody)->setMarginTop(24);
        label(content, tr("pipensx/homebrew/file_policy"));
        for (const auto& file : package_.files)
            label(content, (file.preserve ? tr("pipensx/homebrew/preserve_file") :
                tr("pipensx/homebrew/replace_file")) + " " + file.path, theme::kFontCaption);
        if (!package_.changelog.empty()) {
            label(content, tr("pipensx/homebrew/changelog"), theme::kFontBody)->setMarginTop(24);
            content->addView(markdownView(package_.changelog));
        }
        auto* scroll = new StickScrollingFrame();
        scroll->setGrow(1);
        scroll->setShrink(1);
        scroll->setMinWidth(0);
        scroll->setContentView(content);
        addView(scroll);
    }

    ~HomebrewDetail() override { job_->alive = false; job_->cancelled = true; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* context) override {
        if (busy_ && !job_->cancelled) {
            const uint64_t total = job_->total;
            const uint64_t received = job_->received;
            const int percent = total ? static_cast<int>(std::min<uint64_t>(100, received * 100 / total)) : 0;
            if (percent != percent_) {
                percent_ = percent;
                status_->setText(percent < 100 ? tr("pipensx/homebrew/downloading", percent)
                                              : tr("pipensx/homebrew/installing"));
            }
        }
        brls::Box::draw(vg, x, y, width, height, style, context);
    }

private:
    void confirm() {
        if (busy_) return;
        auto* dialog = new brls::Dialog(tr("pipensx/homebrew/confirm", package_.title, package_.version));
        const auto job = job_;
        dialog->addButton(tr("pipensx/homebrew/install"), [this, job] {
            if (job->alive) start();
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void start() {
        busy_ = true;
        percent_ = -1;
        job_->cancelled = false;
        job_->received = 0;
        job_->total = package_.bytes;
        status_->setVisibility(brls::Visibility::VISIBLE);
        install_->setState(brls::ButtonState::DISABLED);
        cancel_->setVisibility(brls::Visibility::VISIBLE);
        brls::Application::giveFocus(cancel_);
        const auto job = job_;
        brls::async([this, job, package = package_, service = service_] {
            std::string error;
            const bool ok = service->install(package, job->cancelled,
                [job](uint64_t received, uint64_t total) { job->received = received; job->total = total; }, error);
            brls::sync([this, job, ok, error = std::move(error)] {
                if (!job->alive) return;
                busy_ = false;
                install_->setState(brls::ButtonState::ENABLED);
                brls::Application::giveFocus(install_);
                cancel_->setVisibility(brls::Visibility::GONE);
                if (ok) {
                    package_.installedVersion = package_.version;
                    onInstalled_(package_);
                    install_->setText(tr("pipensx/homebrew/reinstall"));
                    status_->setText(tr(package_.systemModule ? "pipensx/homebrew/installed_restart"
                        : "pipensx/homebrew/installed", package_.title));
                } else {
                    status_->setText(job->cancelled ? tr("pipensx/homebrew/cancelled")
                        : tr("pipensx/homebrew/install_failed", error));
                }
            });
        });
    }

    HomebrewPackage package_;
    std::shared_ptr<HomebrewCatalog> service_;
    std::shared_ptr<InstallJob> job_;
    std::function<void(const HomebrewPackage&)> onInstalled_;
    brls::Label* status_;
    theme::Button *install_, *cancel_;
    bool busy_ = false;
    int percent_ = -1;
};

constexpr int kHomebrewColumns = 5;
constexpr float kHomebrewCardHeight = 304;
constexpr float kHomebrewRowHeight = 328;

class HomebrewCard : public brls::Box {
public:
    HomebrewCard() : brls::Box(brls::Axis::COLUMN) {
        setWidth(232); setHeight(kHomebrewCardHeight);
        setPadding(12, 12, 8, 12);
        setAlignItems(brls::AlignItems::CENTER);
        setBackgroundColor(theme::surface());
        setCornerRadius(theme::kRadiusMedium);
        setHighlightCornerRadius(theme::kRadiusMedium);
        setClipsToBounds(true);
        setFocusable(true);
        auto* artwork = new brls::Box();
        artwork->setWidth(208); artwork->setHeight(208);
        artwork->setShrink(0);
        artwork->setAlignItems(brls::AlignItems::CENTER);
        artwork->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder_ = label(artwork, "", theme::kFontTitle);
        placeholder_->setMarginBottom(0);
        placeholder_->setTextColor(theme::textSecondary());
        icon_ = new AsyncRgbaImage();
        icon_->setWidth(208); icon_->setHeight(208);
        icon_->setPositionType(brls::PositionType::ABSOLUTE);
        icon_->setPositionTop(0); icon_->setPositionLeft(0);
        icon_->setCornerRadius(theme::kRadiusMedium);
        icon_->setScalingType(brls::ImageScalingType::FIT);
        artwork->addView(icon_);
        addView(artwork);
        title_ = label(this, "", theme::kFontSmall);
        title_->setWidth(208); title_->setMinHeight(42); title_->setMaxHeight(42); title_->setShrink(0);
        title_->setLineHeight(1.1f);
        title_->setAutoAnimate(false);
        title_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        title_->setMarginTop(8); title_->setMarginBottom(6);
        subtitle_ = label(this, "", theme::kFontCaption);
        subtitle_->setWidth(208); subtitle_->setMinHeight(18); subtitle_->setMaxHeight(18); subtitle_->setShrink(0);
        subtitle_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        subtitle_->setTextColor(theme::textSecondary());
        subtitle_->setSingleLine(true); subtitle_->setMarginBottom(0);
        registerClickAction([this](brls::View*) {
            if (activate_) activate_();
            return true;
        });
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }
    void bind(const HomebrewPackage& package, GameMetadataService* metadata,
              std::function<void()> activate, std::function<void()> focused) {
        setVisibility(brls::Visibility::VISIBLE);
        setFocusable(true);
        activate_ = std::move(activate);
        focused_ = std::move(focused);
        title_->setText(package.title);
        size_t first = 1;
        while (first < package.title.size() && (static_cast<unsigned char>(package.title[first]) & 0xc0) == 0x80) ++first;
        placeholder_->setText(package.title.substr(0, first));
        subtitle_->setText(package.installedVersion.empty()
            ? package.version
            : tr("pipensx/homebrew/installed_version", package.installedVersion));
        setArtworkUrl(icon_, metadata, package.iconUrl, iconUrl_, imageState_);
    }
    void setEmpty() {
        setVisibility(brls::Visibility::INVISIBLE);
        setFocusable(false);
        activate_ = nullptr;
        focused_ = nullptr;
    }
    void onFocusGained() override {
        brls::Box::onFocusGained();
        if (focused_) focused_();
    }
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* context) override {
        if (theme::isCustom())
            theme::drawPlate(vg, x, y, width, height, theme::kRadiusMedium, isFocused());
        title_->setIsWrapping(true);
        brls::Box::draw(vg, x, y, width, height, style, context);
    }
private:
    AsyncRgbaImage* icon_;
    brls::Label *title_, *subtitle_, *placeholder_;
    std::function<void()> activate_, focused_;
    std::string iconUrl_;
    std::shared_ptr<ImageRequestState> imageState_ = std::make_shared<ImageRequestState>();
};

class HomebrewRow : public brls::RecyclerCell {
public:
    explicit HomebrewRow(std::shared_ptr<int> column) : column_(std::move(column)) {
        setAxis(brls::Axis::ROW);
        setHeight(kHomebrewRowHeight);
        setPaddingTop(12);
        setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);
        setFocusable(false);
        for (auto& card : cards_) { card = new HomebrewCard(); addView(card); }
        registerAction("", brls::BUTTON_A, [](brls::View*) { return false; }, true);
    }
    void bind(const std::vector<HomebrewPackage>& packages, size_t start,
              GameMetadataService* metadata,
              const std::function<void(const HomebrewPackage&)>& select) {
        active_ = static_cast<int>(std::min<size_t>(kHomebrewColumns, packages.size() - start));
        for (int i = 0; i < kHomebrewColumns; ++i) {
            if (i >= active_) { cards_[i]->setEmpty(); continue; }
            cards_[i]->bind(packages[start + i], metadata,
                [select, package = packages[start + i]] { select(package); },
                [column = column_, i] { *column = i; });
        }
    }
    brls::View* getDefaultFocus() override {
        if (getVisibility() != brls::Visibility::VISIBLE || active_ <= 0)
            return brls::Box::getDefaultFocus();
        return cards_[std::clamp(*column_, 0, active_ - 1)];
    }
    void frame(brls::FrameContext* context) override {
        // RecyclerFrame restores list separators when it attaches a recycled row.
        setLineTop(0);
        setLineBottom(0);
        brls::RecyclerCell::frame(context);
    }
private:
    std::shared_ptr<int> column_;
    HomebrewCard* cards_[kHomebrewColumns]{};
    int active_ = 0;
};
}

class HomebrewDataSource : public brls::RecyclerDataSource {
public:
    std::vector<HomebrewPackage> packages;
    std::string message;
    GameMetadataService* metadata = nullptr;
    std::function<void(const HomebrewPackage&)> select;
    int numberOfRows(brls::RecyclerFrame*, int) override {
        return std::max<size_t>(1, (packages.size() + kHomebrewColumns - 1) / kHomebrewColumns);
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath) override { return packages.empty() ? 120 : kHomebrewRowHeight; }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        if (packages.empty()) {
            auto* cell = static_cast<TextMessageCell*>(recycler->dequeueReusableCell("Message"));
            cell->setMessage(message);
            return cell;
        }
        auto* cell = static_cast<HomebrewRow*>(recycler->dequeueReusableCell("Homebrew"));
        cell->bind(packages, static_cast<size_t>(index.row) * kHomebrewColumns, metadata, select);
        return cell;
    }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath) override {}
};

HomebrewView::HomebrewView(std::string appRoot, std::string sdRoot, AppSettings* settings,
                           GameMetadataService* metadata)
    : brls::Box(brls::Axis::COLUMN), service_(std::make_shared<HomebrewCatalog>(std::move(appRoot), std::move(sdRoot))),
      settings_(settings), metadata_(metadata), alive_(std::make_shared<std::atomic<bool>>(true)),
      cancelled_(std::make_shared<std::atomic<bool>>(false)) {
    setGrow(1);
    auto* header = new brls::Box(brls::Axis::ROW);
    header->setPadding(12, 28, 12, 28);
    header->setAlignItems(brls::AlignItems::CENTER);
    auto* title = label(header, tr("pipensx/nav/homebrew"), theme::kFontHeading);
    title->setGrow(1);
    title->setMarginBottom(0);
    button(header, tr("pipensx/catalog/search_placeholder"), [this] { search(); });
    button(header, tr("pipensx/catalog/filter_reset"), [this] { query_.clear(); rebuild(); });
    refresh_ = button(header, tr("pipensx/homebrew/refresh"), [this] { update(true); });
    refresh_->setMarginRight(0);
    addView(header);
    status_ = label(this, tr("pipensx/homebrew/loading"));
    status_->setMarginLeft(28); status_->setMarginRight(28);
    recycler_ = new SmoothRecyclerFrame();
    recycler_->setGrow(1); recycler_->setPadding(6, 28, 6, 28);
    recycler_->estimatedRowHeight = kHomebrewRowHeight;
    const auto column = std::make_shared<int>(0);
    recycler_->registerCell("Homebrew", [column] { return new HomebrewRow(column); });
    recycler_->registerCell("Message", [] { return new TextMessageCell(); });
    data_ = new HomebrewDataSource();
    data_->metadata = metadata_;
    data_->message = tr("pipensx/homebrew/loading");
    data_->select = [this](const HomebrewPackage& package) { open(package); };
    recycler_->setDataSource(data_);
    addView(recycler_);
    registerAction(tr("pipensx/catalog/search_placeholder"), brls::BUTTON_X,
                   [this](brls::View*) { search(); return true; });
}

HomebrewView::~HomebrewView() { *alive_ = false; *cancelled_ = true; }

void HomebrewView::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    update(!initialized_);
    initialized_ = true;
}

void HomebrewView::update(bool refresh) {
    if (busy_) return;
    busy_ = true;
    refresh_->setState(brls::ButtonState::DISABLED);
    status_->setText(tr("pipensx/homebrew/loading"));
    const auto alive = alive_, cancelled = cancelled_;
    std::string language = settings_->get().language;
    if (language.empty() || language == "auto") language = brls::Application::getLocale();
    const std::string source = settings_->get().catalogSourceUrl;
    brls::async([this, alive, cancelled, service = service_, language, source, refresh] {
        std::vector<HomebrewPackage> packages;
        std::string error;
        const bool loaded = service->load(packages, language, error);
        if (loaded && refresh) brls::sync([this, alive, packages] {
            if (!*alive) return;
            packages_ = packages;
            rebuild();
        });
        const bool ok = refresh ? service->refresh(packages, language, source, *cancelled, error) : loaded;
        brls::sync([this, alive, ok, loaded, packages = std::move(packages), error = std::move(error)]() mutable {
            if (!*alive) return;
            busy_ = false;
            refresh_->setState(brls::ButtonState::ENABLED);
            if (ok || loaded) packages_ = std::move(packages);
            rebuild();
            status_->setText(ok ? tr("pipensx/homebrew/count", packages_.size())
                : tr(loaded || !packages_.empty() ? "pipensx/homebrew/cached_error" : "pipensx/homebrew/load_failed", error));
        });
    });
}

void HomebrewView::rebuild() {
    if (viewContains(recycler_, brls::Application::getCurrentFocus())) brls::Application::giveFocus(refresh_);
    data_->packages.clear();
    const std::string query = catalogFoldForSearch(query_);
    for (const auto& package : packages_)
        if (query.empty() || catalogFoldForSearch(package.title + " " + package.name + " " + package.author + " " +
                                   package.description).find(query) != std::string::npos)
            data_->packages.push_back(package);
    data_->message = tr("pipensx/homebrew/no_results");
    recycler_->reloadData();
    if (!busy_) status_->setText(tr("pipensx/homebrew/count", data_->packages.size()));
}

void HomebrewView::search() {
    const auto alive = alive_;
    brls::Application::getImeManager()->openForText([this, alive](std::string value) {
        if (!*alive) return;
        query_ = std::move(value);
        rebuild();
    }, tr("pipensx/catalog/search_placeholder"), "", 128, query_, brls::KEYBOARD_DISABLE_NONE);
}

void HomebrewView::open(const HomebrewPackage& package) {
    const auto alive = alive_;
    auto* detail = new HomebrewDetail(package, service_, metadata_,
        [this, alive](const HomebrewPackage& installed) {
            if (!*alive) return;
            for (auto& entry : packages_)
                if (entry.name == installed.name) entry.installedVersion = installed.installedVersion;
            for (auto& entry : data_->packages)
                if (entry.name == installed.name) entry.installedVersion = installed.installedVersion;
            for (auto* cell : visibleCells<HomebrewRow>(recycler_)) {
                const int row = cell->getIndexPath().row;
                const size_t start = static_cast<size_t>(row) * kHomebrewColumns;
                if (row >= 0 && start < data_->packages.size())
                    cell->bind(data_->packages, start, metadata_, data_->select);
            }
        });
    auto* frame = new DetailFrame(detail);
    frame->setTitle(package.title);
    brls::Application::pushActivity(new brls::Activity(frame));
}

} // namespace pipensx::ui
