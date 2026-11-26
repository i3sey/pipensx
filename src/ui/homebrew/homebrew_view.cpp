#include "homebrew_view.hpp"

#include "app/catalog_urls.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/catalog_controls.hpp"
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

bool isSelfPipensx(const HomebrewPackage& package) {
    auto low = [](std::string s) {
        for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return s;
    };
    const std::string name = low(package.name);
    const std::string path = low(package.installedPath);
    return name == "pipensx" || path == "switch/pipensx.nro" ||
           path.rfind("switch/pipensx/", 0) == 0;
}

class HomebrewDetail : public brls::Box {
public:
    HomebrewDetail(HomebrewPackage package, std::shared_ptr<HomebrewCatalog> service,
                   GameMetadataService* metadata,
                   bool installedOnly,
                   std::function<void(const HomebrewPackage&, bool removed)> onChanged)
        : brls::Box(brls::Axis::COLUMN), package_(std::move(package)), service_(std::move(service)),
          job_(std::make_shared<InstallJob>()), installedOnly_(installedOnly),
          onChanged_(std::move(onChanged)) {
        setGrow(1);
        setAxis(brls::Axis::ROW);
        setPadding(24, 40, 24, 40);
        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidth(320);
        left->setShrink(0);
        left->setMarginRight(32);
        auto* icon = new AsyncRgbaImage();
        icon->setWidth(320); icon->setHeight(280);
        icon->setShrink(0);
        icon->setCornerRadius(theme::kRadiusSmall);
        icon->setScalingType(brls::ImageScalingType::FIT);
        icon->setClipsToBounds(false);
        loadImageInto(icon, metadata, package_.iconUrl);
        left->addView(icon);
        install_ = button(left, tr(package_.installedVersion.empty()
            ? "pipensx/homebrew/install" : "pipensx/homebrew/reinstall"), [this] { confirm(); });
        install_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        install_->setHeight(58);
        install_->setShrink(0);
        install_->setMarginTop(14);
        install_->setMarginRight(0);
        uninstall_ = button(left, tr("pipensx/installed/uninstall_action"), [this] { confirmUninstall(); });
        uninstall_->setHeight(48);
        uninstall_->setShrink(0);
        uninstall_->setMarginTop(10);
        uninstall_->setMarginRight(0);
        uninstall_->setVisibility(canUninstall() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        cancel_ = button(left, tr("pipensx/common/cancel"), [this] {
            job_->cancelled = true;
            status_->setText(tr("pipensx/homebrew/cancelling"));
        });
        cancel_->setHeight(48);
        cancel_->setMarginTop(10);
        cancel_->setMarginRight(0);
        cancel_->setVisibility(brls::Visibility::GONE);
        status_ = label(left, package_.installedVersion.empty()
            ? ""
            : tr("pipensx/homebrew/installed_version", package_.installedVersion));
        status_->setMarginTop(12);
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

    void dismiss(std::function<void()> callback) override {
        // A fade draws the description and the icon grid into the same deko3d command buffer.
        brls::Application::popActivity(brls::TransitionAnimation::NONE, std::move(callback));
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
    bool canUninstall() const {
        return installedOnly_ &&
               (!package_.installedVersion.empty() || !package_.installedPath.empty()) &&
               !isSelfPipensx(package_);
    }

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

    void confirmUninstall() {
        if (busy_ || !canUninstall()) return;
        auto* dialog = new brls::Dialog(tr("pipensx/homebrew/uninstall_confirm", package_.title));
        const auto job = job_;
        dialog->addButton(tr("pipensx/installed/uninstall_action"), [this, job] {
            if (job->alive) startUninstall();
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void startUninstall() {
        busy_ = true;
        status_->setVisibility(brls::Visibility::VISIBLE);
        status_->setText(tr("pipensx/homebrew/uninstalling"));
        install_->setState(brls::ButtonState::DISABLED);
        uninstall_->setState(brls::ButtonState::DISABLED);
        const auto job = job_;
        brls::async([this, job, package = package_, service = service_] {
            std::string error;
            const bool ok = service->uninstall(package, error);
            brls::sync([this, job, ok, error = std::move(error)] {
                if (!job->alive) return;
                busy_ = false;
                install_->setState(brls::ButtonState::ENABLED);
                uninstall_->setState(brls::ButtonState::ENABLED);
                if (ok) {
                    package_.installedVersion.clear();
                    package_.installedPath.clear();
                    onChanged_(package_, true);
                    brls::Application::notify(tr(package_.systemModule
                        ? "pipensx/homebrew/uninstalled_restart"
                        : "pipensx/homebrew/uninstalled", package_.title));
                    if (installedOnly_) {
                        dismiss(nullptr);
                        return;
                    }
                    install_->setText(tr("pipensx/homebrew/install"));
                    uninstall_->setVisibility(brls::Visibility::GONE);
                    brls::Application::giveFocus(install_);
                    status_->setText(tr(package_.systemModule
                        ? "pipensx/homebrew/uninstalled_restart"
                        : "pipensx/homebrew/uninstalled", package_.title));
                } else {
                    status_->setText(tr("pipensx/homebrew/uninstall_failed", error));
                }
            });
        });
    }

    void start() {
        busy_ = true;
        percent_ = -1;
        job_->cancelled = false;
        job_->received = 0;
        job_->total = package_.bytes;
        status_->setVisibility(brls::Visibility::VISIBLE);
        install_->setState(brls::ButtonState::DISABLED);
        uninstall_->setVisibility(brls::Visibility::GONE);
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
                    onChanged_(package_, false);
                    install_->setText(tr("pipensx/homebrew/reinstall"));
                    uninstall_->setVisibility(canUninstall() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
                    status_->setText(tr(package_.systemModule ? "pipensx/homebrew/installed_restart"
                        : "pipensx/homebrew/installed", package_.title));
                } else {
                    uninstall_->setVisibility(canUninstall() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
                    status_->setText(job->cancelled ? tr("pipensx/homebrew/cancelled")
                        : tr("pipensx/homebrew/install_failed", error));
                }
            });
        });
    }

    HomebrewPackage package_;
    std::shared_ptr<HomebrewCatalog> service_;
    std::shared_ptr<InstallJob> job_;
    bool installedOnly_ = false;
    std::function<void(const HomebrewPackage&, bool)> onChanged_;
    brls::Label* status_;
    theme::Button *install_, *uninstall_, *cancel_;
    bool busy_ = false;
    int percent_ = -1;
};

constexpr int kHomebrewColumns = 5;
constexpr float kHomebrewCardHeight = 292;
constexpr float kHomebrewRowHeight = 308;

class HomebrewCard : public brls::Box {
public:
    explicit HomebrewCard(bool installedOnly) : brls::Box(brls::Axis::COLUMN), installedOnly_(installedOnly) {
        setWidth(232); setHeight(kHomebrewCardHeight);
        setPadding(10, 12, 8, 12);
        setAlignItems(brls::AlignItems::CENTER);
        setBackgroundColor(theme::surface());
        setCornerRadius(theme::kRadiusMedium);
        setHighlightCornerRadius(theme::kRadiusMedium);
        setClipsToBounds(true);
        setFocusable(true);
        auto* artwork = new brls::Box();
        artwork->setWidth(208); artwork->setHeight(208);
        artwork->setShrink(0);
        artwork->setCornerRadius(theme::kRadiusMedium);
        artwork->setBackgroundColor(theme::graphBg());
        artwork->setClipsToBounds(true);
        artwork->setAlignItems(brls::AlignItems::CENTER);
        artwork->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder_ = label(artwork, "", theme::kFontTitle);
        placeholder_->setSingleLine(true);
        placeholder_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
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
        title_->setWidth(208); title_->setMinHeight(38); title_->setMaxHeight(38); title_->setShrink(0);
        title_->setClipsToBounds(true);
        title_->setLineHeight(1.1f);
        title_->setAutoAnimate(false);
        title_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        title_->setMarginTop(6); title_->setMarginBottom(4);
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
        placeholder_->setText(homebrewMonogram(package.title));
        placeholder_->setVisibility(brls::Visibility::VISIBLE);
        if (installedOnly_) {
            if (package.installedVersion.empty()) {
                subtitle_->setText(tr("pipensx/homebrew/unknown_version"));
                subtitle_->setTextColor(theme::textSecondary());
            } else if (!package.downloadUrl.empty() && !package.version.empty() &&
                       isHomebrewUpdateAvailable(package.installedVersion, package.version)) {
                subtitle_->setText(tr("pipensx/homebrew/installed_update",
                                      package.installedVersion, package.version));
                subtitle_->setTextColor(theme::accent());
            } else {
                subtitle_->setText(tr("pipensx/homebrew/installed_version",
                                      package.installedVersion));
                subtitle_->setTextColor(theme::textSecondary());
            }
        } else {
            subtitle_->setText(package.installedVersion.empty()
                ? package.version
                : tr("pipensx/homebrew/installed_version", package.installedVersion));
            subtitle_->setTextColor(theme::textSecondary());
        }
        setArtworkUrl(icon_, metadata, package.iconUrl, iconUrl_, imageState_);
    }
    void setEmpty() {
        setVisibility(brls::Visibility::INVISIBLE);
        setFocusable(false);
        iconUrl_.clear();
        ++imageState_->generation;
        imageState_->pending = false;
        icon_->resetArtwork();
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
        const brls::Visibility placeholderVis =
            (icon_->getTexture() != 0 && icon_->getAlpha() >= 1.0f)
                ? brls::Visibility::INVISIBLE
                : brls::Visibility::VISIBLE;
        if (placeholder_->getVisibility() != placeholderVis)
            placeholder_->setVisibility(placeholderVis);
        title_->setIsWrapping(true);
        brls::Box::draw(vg, x, y, width, height, style, context);
    }
private:
    bool installedOnly_;
    AsyncRgbaImage* icon_;
    brls::Label *title_, *subtitle_, *placeholder_;
    std::function<void()> activate_, focused_;
    std::string iconUrl_;
    std::shared_ptr<ImageRequestState> imageState_ = std::make_shared<ImageRequestState>();
};

class HomebrewRow : public brls::RecyclerCell {
public:
    HomebrewRow(std::shared_ptr<int> column, bool installedOnly) : column_(std::move(column)) {
        setAxis(brls::Axis::ROW);
        setHeight(kHomebrewRowHeight);
        setPaddingTop(8);
        setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        setLineBottom(0);
        setLineColor(brls::TRANSPARENT);
        setFocusable(false);
        for (auto& card : cards_) { card = new HomebrewCard(installedOnly); addView(card); }
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
    bool loading = true;
    GameMetadataService* metadata = nullptr;
    std::function<void(const HomebrewPackage&)> select;
    int numberOfRows(brls::RecyclerFrame*, int) override {
        if (packages.empty() && loading)
            return 2;
        return std::max<size_t>(1, (packages.size() + kHomebrewColumns - 1) / kHomebrewColumns);
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath) override {
        return (packages.empty() && !loading) ? 120 : kHomebrewRowHeight;
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        if (packages.empty()) {
            if (loading)
                return recycler->dequeueReusableCell("Skeleton");
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
                           GameMetadataService* metadata, bool installedOnly)
    : brls::Box(brls::Axis::COLUMN), service_(std::make_shared<HomebrewCatalog>(std::move(appRoot), std::move(sdRoot))),
      settings_(settings), metadata_(metadata), alive_(std::make_shared<std::atomic<bool>>(true)),
      cancelled_(std::make_shared<std::atomic<bool>>(false)) {
    installedOnly_ = installedOnly;
    setGrow(1);
    auto* header = new brls::Box(brls::Axis::ROW);
    header->setPadding(installedOnly_ ? 4 : 12, 28, installedOnly_ ? 4 : 6, 28);
    header->setAlignItems(brls::AlignItems::CENTER);
    if (!installedOnly_) {
        auto* title = label(header, tr("pipensx/nav/homebrew"), theme::kFontHeading);
        title->setGrow(1);
        title->setMarginBottom(0);
        status_ = label(header, tr("pipensx/homebrew/loading"), theme::kFontCaption);
        status_->setMarginLeft(8);
        status_->setShrink(1);
    } else {
        status_ = label(header, tr("pipensx/homebrew/loading"), theme::kFontSmall);
        status_->setSingleLine(true);
        status_->setGrow(1);
        status_->setShrink(1);
    }
    status_->setTextColor(theme::textSecondary());
    status_->setMarginBottom(0);
    reset_ = catalogChip(tr("pipensx/catalog/filter_reset"), [this] { query_.clear(); rebuild(); });
    header->addView(reset_);
    reset_->setVisibility(brls::Visibility::GONE);
    search_ = new SearchIconButton();
    search_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    search_->setWidth(40);
    search_->setHeight(40);
    search_->setShrink(0.0f);
    search_->setMarginLeft(8);
    search_->setText("");
    search_->registerAction("", brls::BUTTON_A, [this](brls::View*) { search(); return true; }, true);
    search_->addGestureRecognizer(new brls::TapGestureRecognizer(search_));
    header->addView(search_);
    addView(header);
    recycler_ = new SmoothRecyclerFrame();
    recycler_->setGrow(1); recycler_->setPadding(2, 28, 4, 28);
    recycler_->estimatedRowHeight = kHomebrewRowHeight;
    const auto column = std::make_shared<int>(0);
    recycler_->registerCell("Homebrew", [column, installedOnly] { return new HomebrewRow(column, installedOnly); });
    recycler_->registerCell("Message", [] { return new TextMessageCell(); });
    recycler_->registerCell("Skeleton", [] {
        return new SkeletonGridRowCell(kHomebrewColumns, 232.0f, kHomebrewCardHeight,
                                       208.0f, 208.0f, kHomebrewRowHeight);
    });
    data_ = new HomebrewDataSource();
    data_->metadata = metadata_;
    data_->message = tr("pipensx/homebrew/loading");
    data_->select = [this](const HomebrewPackage& package) { open(package); };
    recycler_->setDataSource(data_);
    addView(recyclerHost(recycler_));
    registerAction(tr(installedOnly_ ? "pipensx/common/search" : "pipensx/catalog/search_placeholder"),
                   brls::BUTTON_X, [this](brls::View*) { search(); return true; });
}

HomebrewView::~HomebrewView() { *alive_ = false; *cancelled_ = true; }

void HomebrewView::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    if (!initialized_ || installedOnly_) {
        update(!installedOnly_ && !initialized_);
        initialized_ = true;
    }
}

void HomebrewView::update(bool refresh) {
    if (busy_) return;
    busy_ = true;
    status_->setText(tr("pipensx/homebrew/loading"));
    if (packages_.empty()) {
        data_->loading = true;
        recycler_->reloadData();
    }
    const auto alive = alive_, cancelled = cancelled_;
    std::string language = settings_->get().language;
    if (language.empty() || language == "auto") language = brls::Application::getLocale();
    const std::string source = settings_->get().catalogSourceUrl;
    brls::async([this, alive, cancelled, service = service_, language, source, refresh, installedOnly = installedOnly_] {
        std::vector<HomebrewPackage> packages;
        std::string error;
        const bool loaded = installedOnly ? service->loadInstalled(packages, language, error)
                                          : service->load(packages, language, error);
        if (loaded && refresh) brls::sync([this, alive, packages] {
            if (!*alive) return;
            packages_ = packages;
            rebuild();
        });
        const bool ok = refresh ? service->refresh(packages, language, source, *cancelled, error) : loaded;
        brls::sync([this, alive, ok, loaded, packages = std::move(packages), error = std::move(error)]() mutable {
            if (!*alive) return;
            busy_ = false;
            if (ok || loaded) packages_ = std::move(packages);
            rebuild();
            status_->setText(ok ? tr("pipensx/homebrew/count", data_->packages.size())
                : tr(loaded || !packages_.empty() ? "pipensx/homebrew/cached_error" : "pipensx/homebrew/load_failed", error));
        });
    });
}

void HomebrewView::rebuild() {
    if (query_.empty() && reset_ && viewContains(reset_, brls::Application::getCurrentFocus()))
        brls::Application::giveFocus(search_);
    if (reset_)
        reset_->setVisibility(query_.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    if (viewContains(recycler_, brls::Application::getCurrentFocus()))
        brls::Application::giveFocus(search_);
    data_->packages.clear();
    data_->loading = busy_ && packages_.empty();
    const std::string query = catalogFoldForSearch(query_);
    for (const auto& package : packages_)
        if (query.empty() || catalogFoldForSearch(package.title + " " + package.name + " " + package.author + " " +
                                   package.description).find(query) != std::string::npos)
            data_->packages.push_back(package);
    data_->message = tr((installedOnly_ && query_.empty()) ? "pipensx/installed/homebrew_empty" : "pipensx/homebrew/no_results");
    recycler_->reloadData();
    if (!busy_ || !packages_.empty())
        status_->setText(tr("pipensx/homebrew/count", data_->packages.size()));
}

void HomebrewView::search() {
    const auto alive = alive_;
    brls::Application::getImeManager()->openForText([this, alive](std::string value) {
        if (!*alive) return;
        query_ = std::move(value);
        rebuild();
    }, tr(installedOnly_ ? "pipensx/common/search" : "pipensx/catalog/search_placeholder"),
       "", 128, query_, brls::KEYBOARD_DISABLE_NONE);
}

void HomebrewView::open(const HomebrewPackage& package) {
    const auto alive = alive_;
    if (package.downloadUrl.empty() && !package.installedPath.empty()) {
        const std::string version = package.installedVersion.empty() ? tr("pipensx/homebrew/unknown_version")
            : tr("pipensx/homebrew/installed_version", package.installedVersion);
        auto* dialog = new brls::Dialog(package.title + "\n" + version + "\nsdmc:/" + package.installedPath);
        if (installedOnly_ && !isSelfPipensx(package)) {
            dialog->addButton(tr("pipensx/installed/uninstall_action"), [this, alive, package] {
                auto* confirm = new brls::Dialog(tr("pipensx/homebrew/uninstall_confirm", package.title));
                confirm->addButton(tr("pipensx/installed/uninstall_action"), [this, alive, package] {
                    brls::async([this, alive, service = service_, package] {
                        std::string error;
                        const bool ok = service->uninstall(package, error);
                        brls::sync([this, alive, ok, error = std::move(error), package] {
                            if (!*alive) return;
                            if (ok) {
                                brls::Application::notify(tr("pipensx/homebrew/uninstalled", package.title));
                                update(false);
                            } else {
                                brls::Application::notify(tr("pipensx/homebrew/uninstall_failed", error));
                            }
                        });
                    });
                });
                confirm->addButton(tr("pipensx/common/cancel"), [] {});
                confirm->open();
            });
        }
        dialog->addButton(tr("pipensx/common/close"), [] {});
        dialog->open();
        return;
    }
    auto* detail = new HomebrewDetail(package, service_, metadata_, installedOnly_,
        [this, alive](const HomebrewPackage& changed, bool removed) {
            if (!*alive) return;
            if (removed && installedOnly_) {
                update(false);
                return;
            }
            for (auto& entry : packages_)
                if (entry.name == changed.name) {
                    entry.installedVersion = changed.installedVersion;
                    entry.installedPath = changed.installedPath;
                }
            for (auto& entry : data_->packages)
                if (entry.name == changed.name) {
                    entry.installedVersion = changed.installedVersion;
                    entry.installedPath = changed.installedPath;
                }
            for (auto* cell : visibleCells<HomebrewRow>(recycler_)) {
                const int row = cell->getIndexPath().row;
                const size_t start = static_cast<size_t>(row) * kHomebrewColumns;
                if (row >= 0 && start < data_->packages.size())
                    cell->bind(data_->packages, start, metadata_, data_->select);
            }
        });
    auto* frame = new DetailFrame(detail);
    frame->setTitle(package.title);
    brls::Application::pushActivity(new brls::Activity(frame), brls::TransitionAnimation::NONE);
}

} // namespace pipensx::ui
