#pragma once

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <borealis.hpp>

#include "app/add_release.hpp"
#include "app/app_settings.hpp"
#include "app/catalog_presentation.hpp"
#include "app/game_update_service.hpp"
#include "app/game_update_install.hpp"
#include "ui/i18n.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/favorites_service.hpp"
#include "app/game_update_install.hpp"
#include "app/game_metadata_service.hpp"
#include "app/install_space.hpp"
#include "app/installed_title_service.hpp"
#include "app/magnet_resolver.hpp"
#include "app/torrent_metainfo_fetch.hpp"
#include "app/switch_deploy.hpp"
#include "app/nx_file_types.hpp"
#include "app/port_selection.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/busy_pulse.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/detail_frame.hpp"
#include "ui/common/description_text.hpp"
#include "ui/common/detail_title.hpp"
#include "ui/common/qr_view.hpp"
#include "ui/common/stick_scrolling_frame.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/port_install_dialog.hpp"
#include "ui/detail/screenshot_viewer.hpp"
#include "ui/detail/torrent_selection.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/downloads/details_activity.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// ---------------------------------------------------------------------------
// Install/status button (O5)
// ---------------------------------------------------------------------------
// The primary action button doubles as a live status surface: while a download
// or install is running it keeps its vivid enabled look and dims the
// not-yet-finished remainder (eShop-style progress fill) instead of greying
// out. setProgress(<0) restores the plain button; >=1 leaves it fully filled.
class InstallButton : public brls::Button {
  public:
    void setProgress(float progress) {
        progress_ = progress < 0.0f ? -1.0f : std::min(1.0f, progress);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        // Scrim the pending remainder before the label so the text stays crisp.
        if (progress_ >= 0.0f && progress_ < 1.0f) {
            float radius   = getCornerRadius();
            float doneWidth = width * progress_;
            nvgSave(vg);
            nvgIntersectScissor(vg, x + doneWidth, y, width - doneWidth, height);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, radius);
            nvgFillColor(vg, a(theme::panel()));
            nvgFill(vg);
            nvgRestore(vg);
        }
        brls::Button::draw(vg, x, y, width, height, style, ctx);
    }

  private:
    float progress_ = -1.0f;
};

// ---------------------------------------------------------------------------
// Full-screen game page (eShop-style detail + one-tap install)
// ---------------------------------------------------------------------------

inline bool copyMetainfoFile(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open())
        return false;
    std::ofstream out(dst, std::ios::binary);
    if (!out.is_open())
        return false;
    out << in.rdbuf();
    return out.good();
}

class GameDetailActivity : public brls::Activity {
public:
    // hashLower + failure ("" clears) recorded back into the catalog; onChange
    // asks the catalog list to re-badge.
    using FailureCallback =
        std::function<void(const std::string&, const std::string&)>;
    using ChangeCallback = std::function<void()>;
    // O12: fired (deferred one frame) when the page is torn down after the
    // B-pop, so the catalog can re-seat scroll + focus on the opening card.
    using CloseCallback = std::function<void()>;

    GameDetailActivity(CatalogEntry entry, std::string lastFailure,
                       DownloadManager* manager, GameMetadataService* metadata,
                       InstalledTitleService* installed, AppSettings* settings,
                        FailureCallback onFailure, ChangeCallback onChange,
                        CloseCallback onClose = nullptr,
                        FavoritesService* favorites = nullptr,
                        SwitchDeployService* deploy = nullptr,
                        bool autoInstall = false,
                        bool portInstall = false,
                        AutorunService* autorun = nullptr)
        : entry_(std::move(entry)), lastFailure_(std::move(lastFailure)),
          manager_(manager), metadata_(metadata), installed_(installed),
          settings_(settings), favorites_(favorites),
          deploy_(deploy), autorun_(autorun),
          onFailure_(std::move(onFailure)), onChange_(std::move(onChange)),
          onClose_(std::move(onClose)),
          alive_(std::make_shared<std::atomic<bool>>(true)),
          cancelled_(std::make_shared<std::atomic<bool>>(false)),
          autoInstall_(autoInstall) {
        // The catalog tab still passes portInstall. The Install action is
        // chosen after the file list resolves (cardOneTapUsesPortInstall).
        (void)portInstall;
        const GameMetadata* found =
            metadata_->findByInfoHash(entry_.infoHash, entry_.titleId);
        presentation_ = resolveCatalogPresentation(entry_, found,
            catalogTextPreference(), brls::Application::getLocale());
        titleId_ = presentation_.titleId;
        if (autoInstall_ && !entry_.titleId.empty())
            titleId_ = entry_.titleId;
        playersFact_ = playersFact(found);
        if (playersFact_.empty())
            playersFact_ = catalogPlayersFact(entry_);

        // F3: eShop-style two-column page. Left column is fixed (cover +
        // install button + size/status); the right column scrolls on its own.
        auto* content = new brls::Box(brls::Axis::ROW);
        content->setPadding(24, 40, 24, 40);
        buildLeftColumn(content);
        buildRightColumn(content);
        installBytes_ = entry_.size;
        scheduleSizeMeterRefresh();

        frame_ = new DetailFrame(content);
        frame_->setTitle(detailPageTitle(presentation_.title));
    }

    ~GameDetailActivity() override {
        alive_->store(false);
        cancelled_->store(true);
        timer_.stop();
        stopBusyPulse(primary_);
        stopBusyPulse(statusLabel_);
        if (onChange_)
            onChange_();  // refresh the row badge on the way back
        // O12: deferred a frame — the activity is mid-teardown here and
        // popActivity() has already unwound the focus stack, so the catalog
        // can safely take the focus back now.
        if (onClose_)
            brls::sync([onClose = std::move(onClose_)] { onClose(); });
    }

    brls::View* createContentView() override { return frame_; }

    void onContentAvailable() override {
        if (!presentation_.topicUrl.empty()) {
            registerAction(tr("pipensx/detail/topic_action"), brls::BUTTON_BACK,
                           [this](brls::View*) {
                showTopicQr();
                return true;
            });
        }
        registerAction(tr("pipensx/common/cancel"), brls::BUTTON_Y,
                       [this](brls::View*) {
            if (busy_)
                cancelled_->store(true);
            return true;
        });
        frame_->setActionAvailable(brls::BUTTON_Y, false);
        // Same hotkey as the catalog grid, and the hint is what names the
        // square ★ button above — this page has room for it where the catalog
        // does not.
        if (favorites_) {
            registerAction(tr("pipensx/catalog/action_favorite"),
                           brls::BUTTON_RT, [this](brls::View*) {
                onFavorite();
                return true;
            });
        }
        refreshButtons();
        timer_.setCallback([this] {
            refreshButtons();
            // nsGetStorageSize is a real syscall — re-read it every ~2s, not on
            // every 500ms button tick. Same cadence as the sidebar footer.
            if (++storageTick_ >= 4) {
                storageTick_ = 0;
                scheduleSizeMeterRefresh();
            }
        });
        timer_.start(500);
        brls::Button* focus = primary_;
        if (focus)
            brls::Application::giveFocus(focus);
        if (autoInstall_) {
            autoInstall_ = false;
            // onContentAvailable runs inside pushActivity, before this page
            // joins the stack. A cached torrent can open a picker immediately.
            brls::sync([this, alive = alive_] {
                if (alive->load())
                    startInstall(false);
            });
        }
    }

private:
    static constexpr float kLeftColumnWidth = 260.0f;
    // Leave room for a two-line storage header and a separate status area.
    static constexpr float kCoverHeight = 210.0f;

    // Left column: cover, full-width Install / Select files, size, status (F3).
    //
    // The column does not scroll, so its height is a hard budget: 720 minus the
    // AppletFrame header (88) and footer (73) minus this page's 24px top/bottom
    // padding leaves 511px. Everything below is sized to fit inside it with
    // room to spare — overflow here lands on top of the storage meter and the
    // bottom bar, because Yoga runs with web defaults and nothing clips it.
    void buildLeftColumn(brls::Box* content) {
        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidth(kLeftColumnWidth);
        left->setShrink(0);
        left->setMarginRight(32);
        // Backstop for the budget above: statusLabel_ carries torrent errors and
        // resolve progress, both of which can wrap to an unpredictable number of
        // lines. Same guarantee StorageMeter gives itself.
        left->setClipsToBounds(true);

        if (!presentation_.coverUrl.empty()) {
            // The plate doubles as the placeholder tile until the art arrives
            // (and stays visible as the letterbox behind a 16:9 banner), the
            // same way the catalog card's cover box works.
            auto* plate = new brls::Box();
            plate->setWidth(kLeftColumnWidth);
            plate->setHeight(kCoverHeight);
            plate->setShrink(0);
            plate->setCornerRadius(theme::kRadiusSmall);
            plate->setBackgroundColor(brls::TRANSPARENT);
            auto* cover = new AsyncRgbaImage();
            cover->setWidth(kLeftColumnWidth);
            cover->setHeight(kCoverHeight);
            cover->setPositionType(brls::PositionType::ABSOLUTE);
            cover->setPositionTop(0);
            cover->setPositionLeft(0);
            cover->setCornerRadius(theme::kRadiusSmall);
            cover->setScalingType(brls::ImageScalingType::FIT);
            // FIT letterboxes; with clip on, borealis fills the whole view
            // rect with the pattern and the margin samples clamped edge texels
            // (stretched bands). Clip off draws only the fitted image rect.
            cover->setClipsToBounds(false);
            loadImageInto(
                cover, metadata_, presentation_.coverUrl,
                GameMetadataService::kImageDimCard,
                GameMetadataService::ImagePriority::Current);
            plate->addView(cover);
            left->addView(plate);
        }

        primary_ = new InstallButton();
        primary_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        primary_->setFontSize(theme::kFontBody);
        primary_->setHeight(64);
        primary_->setShrink(0);
        primary_->setMarginTop(16);
        primary_->setText(tr("pipensx/common/install"));
        primary_->registerClickAction([this](brls::View*) {
            onPrimary();
            return true;
        });
        left->addView(primary_);


        // File selection and the wishlist toggle share one row: a fourth full-width
        // button does not fit the column budget, and a square star needs no
        // translation (Russian "В избранном" would not fit it anyway).
        auto* actions = new brls::Box(brls::Axis::ROW);
        actions->setMarginTop(12);
        actions->setHeight(56);
        actions->setShrink(0);

        secondary_ = new brls::Button();
        secondary_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        secondary_->setFontSize(theme::kFontSmall);
        secondary_->setGrow(1);
        secondary_->setHeight(56);
        secondary_->setText(tr("pipensx/torrent/frame_title"));
        secondary_->registerClickAction([this](brls::View*) {
            onSecondary();
            return true;
        });
        actions->addView(secondary_);

        // Wishlist toggle: the same star the catalog grid puts on the card,
        // reachable without hunting for the ZR hotkey.
        if (favorites_) {
            favorite_ = new brls::Button();
            favorite_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
            favorite_->setFontSize(theme::kFontHeading);
            favorite_->setWidth(56);
            favorite_->setHeight(56);
            favorite_->setMarginLeft(8);
            favorite_->setText("★");
            favorite_->registerClickAction([this](brls::View*) {
                onFavorite();
                return true;
            });
            actions->addView(favorite_);
        }
        left->addView(actions);

        // How much of the card this release eats. Seeded from the catalog size
        // and refined to the exact figure once the torrent metadata resolves.
        sizeMeter_ = new StorageMeter(true);
        sizeMeter_->setHeader(storageMeterHeader(
            settings_ ? installTargetFor(settings_->get().installLocation)
                      : manager_->installTarget()));
        sizeMeter_->setMarginTop(16);
        sizeMeter_->setShrink(0);
        left->addView(sizeMeter_);

        statusLabel_ = new brls::Label();
        statusLabel_->setFontSize(theme::kFontCaption);
        statusLabel_->setSingleLine(false);
        statusLabel_->setShrink(0);
        statusLabel_->setTextColor(theme::accent());
        statusLabel_->setText("");
        // Keep long errors/mod warnings readable without compressing the meter.
        auto* statusScroll = new brls::ScrollingFrame();
        statusScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        statusScroll->setHeight(48);
        statusScroll->setShrink(0);
        statusScroll->setMarginTop(8);
        statusScroll->setContentView(statusLabel_);
        left->addView(statusScroll);

        content->addView(left);
    }

    // Right column (scrolls): fact table, screenshots, description. The game
    // title already lives in the AppletFrame header, so do not repeat it here.
    void buildRightColumn(brls::Box* content) {
        auto* right = new brls::Box(brls::Axis::COLUMN);
        right->setPadding(0, 32, 24, 0);

        buildFactsTable(right);

        const std::vector<std::string>& screenshots = presentation_.screenshots;
        if (!screenshots.empty()) {
            auto* shots = new brls::Label();
            shots->setFontSize(theme::kFontSmall);
            shots->setMarginTop(24);
            shots->setMarginBottom(8);
            shots->setTextColor(theme::textSecondary());
            shots->setText(tr("pipensx/detail/screenshots"));
            right->addView(shots);

            auto* rail = new brls::Box(brls::Axis::ROW);
            rail->setHeight(180);
            for (size_t i = 0; i < screenshots.size(); ++i) {
                auto* image = new AsyncRgbaImage();
                image->setWidth(300);
                image->setHeight(170);
                image->setMarginRight(12);
                image->setCornerRadius(theme::kRadiusSmall);
                image->setFocusable(true);
                image->setScalingType(brls::ImageScalingType::FIT);
                image->setClipsToBounds(false);  // no letterbox edge bands
                // The rail is the only focusable thing in this column — the
                // fact table and the description are plain Labels. So the
                // upward walk finds nothing here, then hits content's ROW axis
                // (which ignores UP) and dies at the frame: the rail reads as a
                // dead end. Route UP to the primary action explicitly. DOWN is
                // left to the normal walk, which reaches "show more" when the
                // description has one; a dead end at a column's bottom is what
                // anyone expects anyway.
                image->setCustomNavigationRoute(brls::FocusDirection::UP,
                                                primary_);
                // O6: A opens the fullscreen pager at this shot.
                image->registerClickAction(
                    [this, i](brls::View*) {
                        brls::Application::pushActivity(
                            new ScreenshotViewerActivity(
                                metadata_, presentation_.screenshots, i,
                                presentation_.title));
                        return true;
                    });
                loadImageInto(image, metadata_, screenshots[i]);
                rail->addView(image);
            }
            auto* gallery = new brls::HScrollingFrame();
            gallery->setHeight(190);
            gallery->setContentView(rail);
            right->addView(gallery);
        }

        buildDescription(right);

        std::string warn;
        if (!lastFailure_.empty()) {
            warn = tr("pipensx/detail/last_attempt", lastFailure_);
        } else {
            std::string health = badgeForCatalogHealth(entry_);
            if (!health.empty() && health != tr("pipensx/health/fresh")) {
                warn = tr("pipensx/detail/catalog_health", health);
                if (!entry_.healthReason.empty())
                    warn = tr("pipensx/detail/catalog_health_reason", health,
                              entry_.healthReason);
            }
        }
        if (!warn.empty()) {
            auto* warning = new brls::Label();
            warning->setFontSize(theme::kFontCaption);
            warning->setMarginTop(16);
            warning->setTextColor(theme::warning());
            warning->setText(warn);
            right->addView(warning);
        }

        // Preserve source details only when they differ from the page title.
        if (entry_.title != detailPageTitle(presentation_.title)) {
            auto* release = new brls::Label();
            release->setFontSize(theme::kFontCaption);
            release->setMarginTop(16);
            release->setTextColor(theme::textTertiary());
            release->setSingleLine(false);
            release->setText(tr("pipensx/detail/release_line", entry_.title));
            right->addView(release);
        }

        auto* scroll = new StickScrollingFrame();
        scroll->setGrow(1);
        scroll->setShrink(1);
        scroll->setMinWidth(0);
        scroll->setContentView(right);
        content->addView(scroll);
    }

    void showTopicQr() {
        auto* box = new brls::Box(brls::Axis::COLUMN);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->setPadding(24, 24, 24, 24);
        auto* title = new brls::Label();
        title->setText(tr("pipensx/detail/topic_action"));
        title->setFontSize(theme::kFontHeading);
        title->setMarginBottom(16);
        box->addView(title);
        auto* qr = new QrCodeView(presentation_.topicUrl, true);
        qr->setWidth(288);
        qr->setHeight(288);
        qr->setShrink(0);
        box->addView(qr);
        auto* url = new brls::Label();
        url->setText(presentation_.topicUrl);
        url->setFontSize(theme::kFontSmall);
        url->setMarginTop(16);
        box->addView(url);
        auto* hint = new brls::Label();
        hint->setText(tr("pipensx/detail/topic_qr_hint"));
        hint->setFontSize(theme::kFontSmall);
        hint->setSingleLine(false);
        hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        hint->setMarginTop(8);
        box->addView(hint);
        auto* dialog = new brls::Dialog(box);
        dialog->addButton(tr("pipensx/common/close"), [] {});
        dialog->open();
    }

    // S4: facts as label/value rows instead of one glued string.
    void buildFactsTable(brls::Box* right) {
        auto* table = new brls::Box(brls::Axis::COLUMN);
        table->setMarginTop(8);
        addFactRow(table, tr("pipensx/detail/fact_install_state"),
                   installed_ && installed_->contains(titleId_)
                       ? tr("pipensx/detail/install_state_installed")
                       : tr("pipensx/detail/install_state_not_installed"));
        addFactRow(table, tr("pipensx/detail/fact_installed_version"),
                   installedVersionLabel());
        addFactRow(table, tr("pipensx/detail/fact_available_version"),
                   formatTitleVersion(latestVersionForEntry()));
        // B6: mods live outside ncm — name the folder so a stale mod stops
        // being a guess when an update breaks the launch.
        addFactRow(table, tr("pipensx/detail/fact_mods"), modsFact());
        addFactRow(table, tr("pipensx/detail/fact_developer"),
                   presentation_.developer);
        addFactRow(table, tr("pipensx/detail/fact_publisher"),
                   presentation_.publisher);
        addFactRow(table, tr("pipensx/detail/fact_release"),
                   presentation_.releaseDate);
        if (entry_.metacritic) {
            char score[24];
            std::snprintf(score, sizeof(score), "%g / 100", *entry_.metacritic);
            addFactRow(table, "Metacritic", score);
            addFactRow(table, tr("pipensx/detail/fact_score_source"),
                       tr("pipensx/detail/score_source"));
        }
        addFactRow(table, tr("pipensx/detail/fact_genre"), presentation_.genre);
        addFactRow(table, tr("pipensx/detail/fact_players"), playersFact_);
        addFactRow(table, tr("pipensx/detail/fact_multiplayer"),
                   presentation_.multiplayer);
        addFactRow(table, tr("pipensx/detail/fact_languages"),
                   languagesFact());
        if (preferCatalogNativeText()) {
            addFactRow(table, tr("pipensx/detail/fact_interface_lang"),
                       entry_.interfaceLang);
            addFactRow(table, tr("pipensx/detail/fact_voice_lang"),
                       entry_.voiceLang);
        }
        addFactRow(table, tr("pipensx/detail/fact_performance"),
                   presentation_.performance);
        addFactRow(table, tr("pipensx/detail/fact_size"),
                   entry_.size ? formatBytes(entry_.size)
                               : tr("pipensx/common/unknown"));
        if (catalogLower(presentation_.imageFormat) !=
            catalogLower(packageFact()))
            addFactRow(table, tr("pipensx/detail/fact_package"),
                       packageFact());
        addFactRow(table, tr("pipensx/detail/fact_image_format"),
                   presentation_.imageFormat);
        addFactRow(table, tr("pipensx/detail/fact_version"),
                   presentation_.version);
        addFactRow(table, tr("pipensx/detail/fact_title_id"), titleId_);
        addFactRow(table, tr("pipensx/detail/fact_topic"),
                   presentation_.topicUrl);
        addFactRow(table, tr("pipensx/detail/fact_dlc"), dlcFact());
        if (entry_.isAutorun() && entry_.autorun) {
            const auto& ar = *entry_.autorun;
            addFactRow(table, tr("pipensx/autorun/fact_platform"),
                       tr("pipensx/autorun/fact_platform_val"));
            std::string statusText = autorunCompatibilityBadge(ar);
            if (!ar.status.empty())
                statusText += " (" + ar.status + ")";
            addFactRow(table, tr("pipensx/autorun/fact_status"), statusText);
            if (!ar.translator.empty())
                addFactRow(table, tr("pipensx/autorun/fact_translator"), ar.translator);
            if (!ar.renderer.empty())
                addFactRow(table, tr("pipensx/autorun/fact_renderer"), ar.renderer);
            if (!ar.overclock.empty())
                addFactRow(table, tr("pipensx/autorun/fact_overclock"), ar.overclock);
            if (!ar.executable.empty())
                addFactRow(table, tr("pipensx/autorun/fact_executable"), ar.executable);
            if (!ar.targetFolder.empty())
                addFactRow(table, tr("pipensx/autorun/fact_target_folder"), ar.targetFolder);
            if (!ar.notes.empty())
                addFactRow(table, tr("pipensx/autorun/fact_notes"), ar.notes);
        }
        right->addView(table);
    }

    std::string dlcFact() const {
        if (!installed_ || titleId_.empty() ||
            !installed_->contains(titleId_))
            return {};
        return tr("pipensx/detail/dlc_installed",
                  installed_->dlcCountForBase(titleId_));
    }

    // Structured language fact ("EN, RU", voice appended when it differs).
    // Empty for pre-v2 snapshots, so the row disappears.
    std::string languagesFact() const {
        const std::string interface =
            joinLanguageCodes(presentation_.languagesInterface);
        if (interface.empty())
            return {};
        const std::string voice =
            joinLanguageCodes(presentation_.languagesVoice);
        if (voice.empty() || voice == interface)
            return interface;
        return interface + " (" + tr("pipensx/detail/fact_voice_lang") +
               ": " + voice + ")";
    }

    // Package format fact ("NSZ"). Empty for pre-v2 snapshots.
    std::string packageFact() const {
        std::string out;
        for (char c : presentation_.packageType)
            out += static_cast<char>(
                (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c);
        return out;
    }

    void addFactRow(brls::Box* table, const std::string& name,
                    const std::string& value) {
        if (value.empty())
            return;
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setMarginTop(8);
        auto* key = new brls::Label();
        key->setFontSize(theme::kFontCaption);
        key->setTextColor(theme::textSecondary());
        key->setWidth(205);
        key->setMarginRight(20);
        key->setSingleLine(false);
        key->setShrink(0);
        key->setText(name);
        row->addView(key);
        auto* val = new brls::Label();
        val->setFontSize(theme::kFontCaption);
        val->setTextColor(theme::textSecondary());
        val->setGrow(1);
        val->setShrink(1);
        val->setMinWidth(0);
        val->setSingleLine(false);
        val->setText(catalogLower(value) == "unknown"
                         ? tr("pipensx/common/unknown") : value);
        row->addView(val);
        table->addView(row);
    }

    void buildDescription(brls::Box* right) {
        std::string text = presentation_.description;
        if (text.empty() && entry_.autorun && !entry_.autorun->notes.empty()) {
            text = entry_.autorun->notes;
        }
        text = compactDescription(text);
        if (text.empty()) {
            auto* missing = new brls::Label();
            missing->setFontSize(theme::kFontSmall);
            missing->setMarginTop(24);
            missing->setText(tr("pipensx/detail/no_description"));
            right->addView(missing);
            return;
        }
        auto* desc = new brls::Label();
        desc->setFontSize(theme::kFontSmall);
        desc->setSingleLine(false);
        desc->setMarginTop(24);
        const std::string preview = shortDescription(text);
        const bool truncated = preview != text;
        desc->setText(preview);
        right->addView(desc);
        if (truncated) {
            auto* more = new brls::Button();
            more->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
            more->setFontSize(theme::kFontSmall);
            more->setTextColor(theme::accent());
            more->setMarginTop(4);
            more->setText(tr("pipensx/detail/show_more"));
            more->registerClickAction([desc, more, right, preview,
                                       expanded = std::make_shared<bool>(false),
                                       text = std::move(text)](brls::View*) {
                *expanded = !*expanded;
                desc->setText(*expanded ? text : preview);
                more->setText(tr(*expanded ? "pipensx/detail/show_less"
                                          : "pipensx/detail/show_more"));
                brls::Application::giveFocus(more);
                if (!*expanded) {
                    if (auto* scroll = dynamic_cast<StickScrollingFrame*>(right->getParent()))
                        scroll->revealFocusAfterLayout();
                }
                return true;
            });
            right->addView(more);
        }
    }

    // Find the managed task for this game, if any.
    const DownloadTask* currentTask() {
        cache_.clear();
        const std::string hash = catalogLower(entry_.infoHash);
        auto task = manager_->snapshotUi(hash);
        if (!task)
            return nullptr;
        cache_.push_back(std::move(*task));
        return &cache_.front();
    }

    static float deployFill(const SwitchDeploySnapshot& deploy) {
        if (!deploy.totalBytes)
            return 0.0f;
        return std::min(1.0f, static_cast<float>(deploy.bytesCopied) /
                                  static_cast<float>(deploy.totalBytes));
    }

    static std::string installButtonLabel(const DownloadTask& task) {
        switch (task.status) {
            case DownloadStatus::Queued:
                return tr("pipensx/detail/status_queued");
            case DownloadStatus::Fetching:
                return tr("pipensx/detail/status_fetching",
                          percentOf(static_cast<float>(task.fetchProgress)));
            case DownloadStatus::Checking:
            case DownloadStatus::Verifying: {
                const RecoveryAccount account = recoveryAccountOf(task);
                if (account.verifyingSaved || account.downloadedBytes > 0)
                    return tr("pipensx/downloads/axis_verifying");
                return tr("pipensx/downloads/status_checking");
            }
            case DownloadStatus::Downloading: {
                if (task.source == TaskSource::Torrent && task.peers == 0 &&
                    task.speedBytesPerSecond == 0)
                    return tr("pipensx/detail/finding_peers");
                return tr("pipensx/detail/status_downloading",
                          percentOf(progressOf(task)));
            }
            case DownloadStatus::Installing:
            case DownloadStatus::Committing: {
                return tr("pipensx/detail/status_installing",
                          percentOf(installProgressOf(task)));
            }
            case DownloadStatus::Paused: {
                int pct = percentOf(progressOf(task));
                return pct > 0
                    ? tr("pipensx/detail/status_paused_percent", pct)
                    : tr("pipensx/detail/status_paused");
            }
            case DownloadStatus::Completed:
                if (task.mode == TransferMode::DownloadOnly ||
                    task.mode == TransferMode::PortInstall)
                    return tr("pipensx/detail/status_saved_to_sd");
                return tr("pipensx/detail/status_downloaded");
            case DownloadStatus::Installed:
                return tr("pipensx/detail/status_installed");
            case DownloadStatus::Error: {
                int pct = percentOf(progressOf(task));
                return pct > 0
                    ? tr("pipensx/detail/status_error_percent", pct)
                    : tr("pipensx/detail/status_error");
            }
            case DownloadStatus::Removing:
                return tr("pipensx/detail/status_removing");
        }
        return tr("pipensx/common/install");
    }

    // Fill fraction the status button paints while a task is live: install
    // phases track installed bytes, everything else tracks downloaded bytes.
    static float progressForButton(const DownloadTask& task) {
        switch (task.status) {
            case DownloadStatus::Fetching:
                return std::clamp(static_cast<float>(task.fetchProgress),
                                  0.0f, 1.0f);
            case DownloadStatus::Installing:
            case DownloadStatus::Committing:
                return streamInstallProgressOf(task);
            case DownloadStatus::Completed:
            case DownloadStatus::Installed:
                return 1.0f;
            case DownloadStatus::Queued:
                return 0.0f;
            default:
                return task.mode == TransferMode::StreamInstall
                           ? streamInstallProgressOf(task)
                           : progressOf(task);
        }
    }

    // Repaint the install-size bar against the current free space. installBytes_
    // starts as the catalog-declared size and becomes exact once the torrent
    // metadata has been read (see finishImport).
    pipensx::install::InstallStorageTarget currentInstallTarget() const {
        return settings_
            ? installTargetFor(settings_->get().installLocation)
            : manager_->installTarget();
    }

    void scheduleSizeMeterRefresh() {
        if (!sizeMeter_)
            return;
        const auto target = currentInstallTarget();
        sizeMeter_->setHeader(storageMeterHeader(target));
        if (!storageMeterTargetValid_ || storageMeterTarget_ != target)
            sizeMeter_->setUnavailable();
        if (storageQueryInFlight_) {
            storageRefreshPending_ = true;
            return;
        }
        storageQueryInFlight_ = true;
        storageQueryTarget_ = target;
        const std::string root = manager_->rootPath();
        auto alive = alive_;
        brls::async([this, alive, target, root] {
            const uint64_t startedUs = telemetry_enabled() ? now_us() : 0;
            const pipensx::StorageSpaceSnapshot storage =
                pipensx::queryInstallStorageSpace(target, root);
            if (startedUs)
                telemetry_log(
                    "ui", "detail", "event=storage duration_us=%llu",
                    (unsigned long long)(now_us() - startedUs));
            brls::sync([this, alive, target, storage] {
                if (!alive->load())
                    return;
                storageQueryInFlight_ = false;
                if (storageQueryTarget_ == target &&
                    currentInstallTarget() == target) {
                    storageMeterTarget_ = target;
                    storageMeterTargetValid_ = true;
                    if (storage.available) {
                        const uint64_t margin =
                            target == pipensx::install::InstallStorageTarget::Nand
                                ? pipensx::install::kNandSafetyMarginBytes
                                : 0;
                        sizeMeter_->setGameEstimate(
                            storage.totalBytes, storage.freeBytes,
                            installBytes_,
                            storage.freeBytes < installBytes_ + margin,
                            sizeExact_);
                    } else {
                        sizeMeter_->setUnavailable();
                    }
                } else {
                    storageRefreshPending_ = true;
                }
                if (storageRefreshPending_) {
                    storageRefreshPending_ = false;
                    scheduleSizeMeterRefresh();
                }
            });
        });
    }

    // Pulse Install + status while magnet/debrid resolve is in flight (#19).
    void setBusy(bool busy) {
        busy_ = busy;
        frame_->setActionAvailable(brls::BUTTON_Y, busy);
        if (busy) {
            startBusyPulse(primary_);
            startBusyPulse(statusLabel_);
        } else {
            stopBusyPulse(primary_);
            stopBusyPulse(statusLabel_);
        }
    }

    // Star/unstar this game. The cap is reported in the user's words; any
    // other failure is a write error worth showing verbatim.
    void onFavorite() {
        if (!favorites_)
            return;
        const bool starred = favorites_->contains(entry_.infoHash);
        if (!starred &&
            favorites_->items().size() >= FavoritesService::kMaxFavorites) {
            brls::Application::notify(
                tr("pipensx/catalog/favorites_full",
                   FavoritesService::kMaxFavorites));
            return;
        }
        std::string error;
        favorites_->toggle(entry_.infoHash, presentation_.title, error);
        if (!error.empty()) {
            brls::Application::notify(
                tr("pipensx/catalog/favorites_failed", error));
            return;
        }
        refreshFavoriteButton();
        // The catalog patches this entry's star (or its active favorites
        // filter) on this notification.
        if (onChange_)
            onChange_();
    }

    // The glyph never changes \u2014 an outline star (U+2606) is not exercised by
    // any PC test baseline, so there is no evidence the console shared font
    // carries it. State rides on the fill instead, the same way the catalog
    // header's \u2605 filter chip reads as on/off.
    void refreshFavoriteButton() {
        if (!favorite_)
            return;
        const bool starred = favorites_ &&
                             favorites_->contains(entry_.infoHash);
        favorite_->setStyle(starred ? &brls::BUTTONSTYLE_PRIMARY
                                    : &brls::BUTTONSTYLE_DEFAULT);
    }

    // Reflect live task state on the buttons. Skipped while resolving so the
    // inline progress text isn't clobbered.
    void refreshButtons() {
        refreshFavoriteButton();
        if (busy_)
            return;
        const DownloadTask* task = currentTask();
        const SwitchDeploySnapshot deployState =
            deploy_ ? deploy_->snapshot() : SwitchDeploySnapshot{};
        const bool deploying = task && deployState.active() &&
                               deployState.taskId == task->id;
        if (task) {
            operationMessage_.clear();
            // O5: the button is the status surface — it stays vivid (enabled)
            // and shows a progress fill instead of greying out. Paused/Error
            // become an actionable "Resume".
            bool actionable = !deploying &&
                              (task->status == DownloadStatus::Paused ||
                               task->status == DownloadStatus::Error);
            if (actionable) {
                setTextIfChanged(primary_, tr("pipensx/common/resume"));
                primary_->setProgress(-1.0f);
            } else if (deploying) {
                const float fill = deployFill(deployState);
                setTextIfChanged(
                    primary_,
                    tr("pipensx/detail/status_installing", percentOf(fill)));
                primary_->setProgress(fill);
            } else {
                setTextIfChanged(primary_, installButtonLabel(*task));
                primary_->setProgress(progressForButton(*task));
            }
            primary_->setState(brls::ButtonState::ENABLED);
            setTextIfChanged(secondary_, tr("pipensx/torrent/frame_title"));
            secondary_->setState((deploying || task->status == DownloadStatus::Committing)
                                     ? brls::ButtonState::DISABLED
                                     : brls::ButtonState::ENABLED);
            // B6: an install/update failure used to leave only the raw
            // backend line; append the actionable hint (mods / firmware /
            // sigpatches) so a post-update "won't boot" names the fix.
            if (task->status == DownloadStatus::Error && !task->error.empty())
                setTextIfChanged(statusLabel_,
                                 withUpdateFailureHint(task->error));
            else if (task->status == DownloadStatus::Completed &&
                     (task->mode == TransferMode::DownloadOnly ||
                      task->mode == TransferMode::PortInstall))
                setTextIfChanged(statusLabel_,
                                 tr("pipensx/downloads/status_completed_saved"));
        } else {
            setTextIfChanged(primary_, tr(installed_ && installed_->contains(titleId_)
                ? "pipensx/detail/install_extras" : "pipensx/common/install"));
            primary_->setProgress(-1.0f);
            primary_->setState(brls::ButtonState::ENABLED);
            setTextIfChanged(secondary_, tr("pipensx/torrent/frame_title"));
            secondary_->setState(brls::ButtonState::ENABLED);
            if (!operationMessage_.empty())
                setTextIfChanged(statusLabel_, operationMessage_);
            else if (installed_ && installed_->contains(titleId_)) {
                // B6: an installed title with LayeredFS mods keeps a
                // standing warning — the update may have been what broke
                // the launch, and the fix is the mods folder, not a
                // reinstall.
                if (hasModsForTitle() && !titleId_.empty())
                    setTextIfChanged(
                        statusLabel_,
                        tr("pipensx/detail/update_mods_warning", titleId_));
                else {
                const std::string latestXyz =
                    formatTitleVersion(latestVersionForEntry());
                if (!latestXyz.empty() &&
                    titleVersionIsNewer(latestVersionForEntry(),
                                        installedVersionForTitle()))
                    setTextIfChanged(
                        statusLabel_,
                        tr("pipensx/detail/update_available_hint", latestXyz));
                else
                    setTextIfChanged(statusLabel_,
                                     tr("pipensx/detail/installed_hint"));
                }
            } else
                setTextIfChanged(statusLabel_, "");
        }
    }

    void onPrimary() {
        if (busy_)
            return;
        const DownloadTask* task = currentTask();
        if (task) {
            if (task->status == DownloadStatus::Paused ||
                task->status == DownloadStatus::Error)
                manager_->resume(task->id);
            else if (task->status == DownloadStatus::Completed &&
                     (task->mode == TransferMode::DownloadOnly ||
                      task->mode == TransferMode::PortInstall) &&
                     taskHasPortOrArchiveFiles(manager_->rootPath(), *task)) {
                const std::string taskId = task->id;
                openPortNoticeDialog([this, taskId] {
                    brls::Application::pushActivity(
                        new DetailsActivity(taskId, manager_, deploy_, settings_));
                });
            } else
                // O5: tapping the live status button opens the download details.
                brls::Application::pushActivity(
                    new DetailsActivity(task->id, manager_, deploy_, settings_));
            return;
        }
        if (entry_.isAutorun()) {
            const std::string sdRoot = autorun_ ? autorun_->sdRoot() : "sdmc:";
            if (!isAutorunInstalled(sdRoot)) {
                auto* dialog = new brls::Dialog(
                    tr("pipensx/autorun/runtime_required_desc"));
                dialog->addButton(tr("pipensx/autorun/install_runtime"), [this] {
                    setBusy(true);
                    cancelled_->store(false);
                    primary_->setText(tr("pipensx/autorun/runtime_checking"));
                    primary_->setProgress(0.0f);
                    primary_->setState(brls::ButtonState::DISABLED);
                    secondary_->setState(brls::ButtonState::DISABLED);
                    auto alive = alive_;
                    auto cancelled = cancelled_;
                    AutorunService* autorun = autorun_;
                    brls::async([this, alive, autorun, cancelled] {
                        auto progress = [this, alive](bool extracting, uint64_t current, uint64_t total) {
                            const float fill = total ? std::min(1.0f, float(current) / total) : 0.0f;
                            const auto text = extracting
                                ? tr("pipensx/autorun/runtime_extracting", current / (1024 * 1024))
                                : (total ? tr("pipensx/autorun/runtime_downloading", percentOf(fill))
                                         : tr("pipensx/autorun/runtime_downloading_bytes", current / (1024 * 1024)));
                            brls::sync([this, alive, fill, text] {
                                if (!alive->load()) return;
                                primary_->setText(text);
                                primary_->setProgress(fill);
                                statusLabel_->setText(text + tr("pipensx/detail/cancel_hint"));
                            });
                        };
                        std::string error;
                        bool ok = false;
                        if (autorun) {
                            AutorunUpdateCheck check = autorun->checkForUpdates();
                            if (check.ok && !check.release.archiveUrl.empty()) {
                                ok = autorun->downloadAndInstallRuntime(check.release, error, *cancelled,
                                    [progress](uint64_t current, uint64_t total) { progress(true, current, total); },
                                    [progress](uint64_t current, uint64_t total) { progress(false, current, total); });
                            } else if (!check.error.empty()) {
                                error = check.error;
                            } else {
                                error = "AutoRun runtime download is unavailable.";
                            }
                        } else {
                            error = "AutoRun service is unavailable.";
                        }
                        brls::sync([this, alive, ok, error] {
                            if (!alive->load())
                                return;
                            setBusy(false);
                            refreshButtons();
                            if (ok) {
                                operationMessage_ = tr("pipensx/autorun/runtime_installed");
                                statusLabel_->setText(operationMessage_);
                                brls::Application::notify(tr("pipensx/autorun/runtime_installed"));
                            } else {
                                operationMessage_ = tr("pipensx/autorun/runtime_install_failed", error);
                                statusLabel_->setText(operationMessage_);
                                brls::Application::notify(operationMessage_);
                            }
                        });
                    });
                });
                dialog->addButton(tr("pipensx/common/cancel"), [] {});
                dialog->open();
                return;
            }
        }
        // One-tap install: resolve, then queue silently (picker only on Select files).
        startInstall(false);
    }

    void onSecondary() {
        if (busy_)
            return;
        const DownloadTask* task = currentTask();
        const SwitchDeploySnapshot deployState =
            deploy_ ? deploy_->snapshot() : SwitchDeploySnapshot{};
        const bool deploying = task && deployState.active() &&
                               deployState.taskId == task->id;
        if (task && (deploying || task->status == DownloadStatus::Committing))
            return;
        // Select files: always open the per-file picker after resolve.
        startInstall(true);
    }

    // One-tap: resolve the magnet inline, then import immediately (no second
    // dialog) unless forcePicker is set (the "Select files" path), which always
    // opens the per-file selection screen after resolve.
    void startInstall(bool forcePicker) {
        if (busy_)
            return;
        if (debridModeActive(settings_)) {
            startDebridInstall(TransferMode::StreamInstall, forcePicker);
            return;
        }
        const DownloadTask* existing = currentTask();
        if (existing && !existing->metainfoPath.empty() &&
            ::access(existing->metainfoPath.c_str(), R_OK) == 0) {
            uint32_t serial = gCatalogTempSerial.fetch_add(1);
            std::string tmp = manager_->rootPath() + "/_catalog_tmp_" +
                              catalogLower(entry_.infoHash) + "_" +
                              std::to_string(serial) + ".torrent";
            if (copyMetainfoFile(existing->metainfoPath, tmp)) {
                finishImport(tmp, forcePicker, {});
                return;
            }
        }
        setBusy(true);
        operationMessage_.clear();
        cancelled_->store(false);
        primary_->setState(brls::ButtonState::DISABLED);
        secondary_->setState(brls::ButtonState::DISABLED);
        primary_->setText(tr("pipensx/detail/resolving"));
        statusLabel_->setText(tr("pipensx/detail/finding_peers") +
                              tr("pipensx/detail/cancel_hint"));

        auto alive = alive_;
        auto cancelled = cancelled_;
        uint32_t serial = gCatalogTempSerial.fetch_add(1);
        std::string tmp = manager_->rootPath() + "/_catalog_tmp_" +
                          catalogLower(entry_.infoHash) + "_" +
                          std::to_string(serial) + ".torrent";
        std::string magnet = entry_.magnetUri;
        std::vector<uint8_t> infoDict = entry_.infoDict;
        std::string telemetryTag = catalogLower(entry_.infoHash);
        uint64_t startedMs = now_ms();
        brls::async([this, alive, cancelled, magnet, infoDict, tmp,
                     forcePicker, telemetryTag, startedMs] {
            std::string err;
            MagnetResolver resolver;
            auto progress = [this, alive, last = std::string()](
                                const pipensx::MagnetProgress& p) mutable {
                std::string text;
                switch (p.stage) {
                    case pipensx::MagnetProgress::Stage::FindingPeers:
                        text = tr("pipensx/detail/finding_peers");
                        break;
                    case pipensx::MagnetProgress::Stage::Connecting:
                        text = tr("pipensx/detail/contacting_peer",
                                  p.peerIndex, p.peerCount);
                        break;
                    case pipensx::MagnetProgress::Stage::FetchingMetadata:
                        text = tr("pipensx/detail/fetching_metadata",
                                  p.completedPieces, p.totalPieces);
                        break;
                    case pipensx::MagnetProgress::Stage::Validating:
                        text = tr("pipensx/detail/validating");
                        break;
                }
                if (text == last)
                    return;
                last = text;
                brls::sync([this, alive, text] {
                    if (alive->load() && busy_) {
                        primary_->setText(text);
                        statusLabel_->setText(text + tr("pipensx/detail/cancel_hint"));
                    }
                });
            };
            std::vector<uint8_t> initialPeers;
            bool ok = resolver.resolveToFile(
                magnet, tmp, *cancelled, progress, err, &initialPeers,
                infoDict.empty() ? nullptr : &infoDict);
            telemetry_log("magnet", telemetryTag.c_str(),
                          "event=resolve ok=%d cancelled=%d duration_ms=%llu "
                          "verified_peers=%u",
                          ok ? 1 : 0, cancelled->load() ? 1 : 0,
                          (unsigned long long)(now_ms() - startedMs),
                          static_cast<unsigned>(initialPeers.size() / 6));
            brls::sync([this, alive, ok, err, tmp, forcePicker,
                        initialPeers = std::move(initialPeers)]() mutable {
                if (!alive->load()) {
                    ::unlink(tmp.c_str());
                    return;
                }
                setBusy(false);
                std::string hash = catalogLower(entry_.infoHash);
                if (!ok) {
                    std::string reason = classifyResolveFailure(err);
                    if (onFailure_)
                        onFailure_(hash, reason);
                    diagnostic_error("magnet", hash.c_str(), "error=%s",
                                     err.c_str());
                    operationMessage_ = reason;
                    refreshButtons();
                    brls::Application::notify(err);
                    ::unlink(tmp.c_str());
                    return;
                }
                if (onFailure_)
                    onFailure_(hash, "");  // clear stale failure
                finishImport(tmp, forcePicker, std::move(initialPeers));
            });
        });
    }

    struct PortImportPending {
        std::string torrentPath;
        TorrentPreview preview;
        std::vector<uint8_t> peers;
        DebridImport debrid;
        bool debridMode = false;
        DebridProviderKind providerKind = DebridProviderKind::TorBox;
        std::string debridKey;
        std::string debridId;
    };

    static std::string portLayoutStatus(const TorrentPreview& preview) {
        return torrentHasLayeredFsPayload(preview)
            ? tr("pipensx/port_install/layout_layered")
            : torrentPortLayoutDetected(preview)
                ? tr("pipensx/port_install/layout_detected")
                : tr("pipensx/port_install/layout_missing");
    }

    void beginResolvedPortInstall(
        std::shared_ptr<PortImportPending> pending) {
        if (!pending)
            return;
        setBusy(true);
        operationMessage_.clear();
        cancelled_->store(false);
        auto host = std::make_shared<PortInstallDialogHost>();
        auto alive = alive_;
        auto cancelled = cancelled_;
        const bool layoutOk = torrentPortLayoutDetected(pending->preview);
        *host = openPortInstallDialog(
            [this, alive, pending] {
                if (!alive->load())
                    return;
                finishPortImport(*pending, true);
            },
            [this, alive, pending] {
                if (alive->load()) finishPortImport(*pending, false);
            },
            [this, alive, cancelled, pending] {
                cancelled->store(true);
                if (!pending->torrentPath.empty())
                    ::unlink(pending->torrentPath.c_str());
                if (pending->debridMode && !pending->debridId.empty())
                    removeDebridTransferAsync(pending->providerKind,
                                              pending->debridKey,
                                              pending->debridId);
                if (alive->load()) {
                    setBusy(false);
                    refreshButtons();
                }
            });
        setPortInstallReady(*host, portLayoutStatus(pending->preview));
    }

    void finishPortImport(PortImportPending& pending, bool automatic) {
        std::vector<uint8_t> mask =
            automatic ? selectPortInstallActions(pending.preview)
                      : std::vector<uint8_t>(pending.preview.files.size(),
                                            static_cast<uint8_t>(FileAction::Download));
        std::string id;
        std::string err;
        bool ok = false;
        if (pending.debridMode) {
            pending.debrid.fileSelection = mask;
            pending.debrid.packageCount = 0;
            for (size_t i = 0; i < mask.size() &&
                               i < pending.preview.files.size(); ++i) {
                if (pending.preview.files[i].package &&
                    mask[i] != static_cast<uint8_t>(FileAction::Skip))
                    ++pending.debrid.packageCount;
            }
            pending.debrid.mode = automatic ? TransferMode::PortInstall : TransferMode::DownloadOnly;
            if (!automatic) pending.debrid.packageCount = 0;
            ok = manager_->importDebrid(pending.debrid, id, err);
            if (!ok && !pending.debridId.empty())
                removeDebridTransferAsync(pending.providerKind,
                                          pending.debridKey,
                                          pending.debridId);
        } else {
            ok = manager_->importTorrentActions(pending.torrentPath, mask, id,
                                                err, pending.peers);
            ::unlink(pending.torrentPath.c_str());
            pending.torrentPath.clear();
        }
        if (ok) {
            if (automatic && deploy_)
                deploy_->armAutoCopy(id);
            const auto message = tr(automatic ? "pipensx/port_install/queued"
                                              : "pipensx/port_install/download_queued");
            statusLabel_->setText(message);
            brls::Application::notify(message);
            if (onChange_)
                onChange_();
        } else if (catalogLower(err).find("already in the download manager") !=
                   std::string::npos) {
            if (automatic && deploy_)
                deploy_->armAutoCopy(catalogLower(entry_.infoHash));
            statusLabel_->setText(tr("pipensx/detail/already_in_downloads"));
            if (onChange_)
                onChange_();
        } else {
            operationMessage_ = err;
            brls::Application::notify(err.empty()
                ? tr("pipensx/detail/no_installable") : err);
        }
        setBusy(false);
        refreshButtons();
    }

    void startDebridInstall(TransferMode mode, bool forcePicker = false) {
        if (busy_ || !ensureDebridLinked(settings_, manager_))
            return;
        setBusy(true);
        cancelled_->store(false);
        primary_->setState(brls::ButtonState::DISABLED);
        secondary_->setState(brls::ButtonState::DISABLED);
        primary_->setText(tr("pipensx/debrid/submitting"));
        statusLabel_->setText(tr("pipensx/debrid/sending_magnet"));

        auto alive = alive_;
        auto cancelled = cancelled_;
        const AppSettingsData values = settings_->get();
        const DebridProviderKind providerKind = values.debridProvider;
        const bool keepUncached = values.keepUncachedDebrid;
        log_msg("[debrid-intent] source=switch phase=install-click hash=%s provider=%d\n",
                entry_.infoHash.c_str(), static_cast<int>(providerKind));
        const std::string key = activeDebridKey(values);
        const CatalogEntry entry = entry_;
        const std::string root = manager_->rootPath();
        const uint32_t serial = gCatalogTempSerial.fetch_add(1);
        brls::async([this, alive, cancelled, providerKind, key, entry, mode,
                     forcePicker, root, serial, keepUncached] {
            auto provider = makeDebridProvider(providerKind, key);
            std::string debridId;
            std::string error;
            DebridInfo info;
            const std::string tmp =
                root + "/_debrid_tmp_" + catalogLower(entry.infoHash) + "_" +
                std::to_string(serial) + ".torrent";
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(60);
            auto onStage = [this, alive](DebridCreateStage stage) {
                std::string text;
                switch (stage) {
                    case DebridCreateStage::FetchingTorrent:
                        text = tr("pipensx/debrid/fetching_torrent");
                        break;
                    case DebridCreateStage::UploadingTorrent:
                        text = tr("pipensx/debrid/submitting");
                        break;
                    case DebridCreateStage::SendingMagnet:
                    default:
                        text = tr("pipensx/debrid/sending_magnet");
                        break;
                }
                brls::sync([this, alive, text] {
                    if (alive->load() && busy_)
                        statusLabel_->setText(text);
                });
            };
            const bool ok = createDebridWithMetainfoFallback(
                *provider, entry.magnetUri, catalogLower(entry.infoHash),
                entry.infoDict, tmp, *cancelled, deadline, debridId, info,
                error, onStage, keepUncached);
            log_msg("[DEBUG-debrid-picker] create ok=%d id=%s files=%u\n",
                    ok ? 1 : 0, debridId.c_str(),
                    static_cast<unsigned>(info.files.size()));
            brls::sync([this, alive, cancelled, ok, error, debridId, info,
                        entry, providerKind, key, mode, forcePicker, keepUncached] {
                if (!alive->load() || cancelled->load()) {
                    if (!debridId.empty())
                        removeDebridTransferAsync(providerKind, key, debridId);
                    if (alive->load()) {
                        setBusy(false);
                        refreshButtons();
                    }
                    return;
                }
                setBusy(false);
                if (!ok) {
                    const bool retained = keepUncached && !debridId.empty();
                    if (!retained && !debridId.empty())
                        removeDebridTransferAsync(providerKind, key, debridId);
                    operationMessage_ = retained
                        ? tr("pipensx/debrid/kept_for_caching")
                        : (error.empty() ? tr("pipensx/debrid/magnet_unavailable") : error);
                    refreshButtons();
                    brls::Application::notify(operationMessage_);
                    return;
                }
                DebridImport import;
                import.infoHash = catalogLower(entry.infoHash);
                import.name = info.name.empty() ? entry.title : info.name;
                import.totalBytes = info.bytes ? info.bytes : entry.size;
                import.provider = providerKind;
                import.debridId = debridId;
                import.mode = mode;
                TorrentPreview layoutPreview = previewFromDebridFiles(
                    info, entry.title, entry.size);
                import.name = layoutPreview.name;
                import.totalBytes = layoutPreview.totalBytes;
                if (forcePicker && !layoutPreview.files.empty()) {
                    StreamSelection selection = settings_->get().streamSelection;
                    log_msg("[DEBUG-debrid-picker] push id=%s files=%u\n",
                            debridId.c_str(), layoutPreview.fileCount);
                    brls::Application::pushActivity(new TorrentSelectionActivity(
                        manager_, "", std::move(layoutPreview),
                        TransferMode::StreamInstall, selection, {}, import,
                        [providerKind, key, debridId] {
                            removeDebridTransferAsync(providerKind, key, debridId);
                        }, deploy_));
                    log_msg("[DEBUG-debrid-picker] push complete id=%s\n",
                            debridId.c_str());
                    return;
                }
                auto queueResolved = [this, alive, import, layoutPreview, entry,
                                      providerKind, key, debridId]() mutable {
                const OneTapPlan plan =
                    planCatalogOneTap(layoutPreview, oneTapContext());
                if (plan.mode == TransferMode::AutoRunInstall) {
                    import.fileSelection = plan.fileSelection;
                    import.packageCount = 0;
                    import.mode = TransferMode::AutoRunInstall;
                    if (entry.autorun)
                        import.autorunFolder = entry.autorun->targetFolder;
                    std::string id;
                    std::string importError;
                    if (!manager_->importDebrid(import, id, importError)) {
                        removeDebridTransferAsync(providerKind, key, debridId);
                        operationMessage_ = importError;
                        brls::Application::notify(importError);
                    } else {
                        if (entry.autorun) {
                            std::string profileError;
                            auto profile = *entry.autorun;
                            profile.coverUrl = presentation_.coverUrl;
                            saveAutorunProfile(manager_->rootPath(), id, profile, profileError);
                        }
                        if (deploy_)
                            deploy_->armAutoCopy(id);
                        statusLabel_->setText(tr("pipensx/autorun/queued"));
                        brls::Application::notify(tr("pipensx/autorun/queued"));
                        if (onChange_)
                            onChange_();
                    }
                    refreshButtons();
                    return;
                }
                if (plan.outcome == OneTapOutcome::QueuePort) {
                    auto pending = std::make_shared<PortImportPending>();
                    pending->debridMode = true;
                    pending->providerKind = providerKind;
                    pending->debridKey = key;
                    pending->debridId = debridId;
                    pending->preview = std::move(layoutPreview);
                    pending->debrid = import;
                    pending->debrid.infoHash = catalogLower(entry.infoHash);
                    pending->debrid.name = pending->preview.name;
                    pending->debrid.totalBytes = pending->preview.totalBytes;
                    pending->debrid.provider = providerKind;
                    pending->debrid.debridId = debridId;
                    beginResolvedPortInstall(pending);
                    return;
                }
                if (plan.outcome == OneTapOutcome::NeedsChooser) {
                    operationMessage_ = oneTapBlockText(plan.block);
                    refreshButtons();
                    brls::Application::notify(operationMessage_);
                    brls::Application::pushActivity(new TorrentSelectionActivity(
                        manager_, "", std::move(layoutPreview),
                        plan.chooserMode, settings_->get().streamSelection, {},
                        import,
                        [providerKind, key, debridId] {
                            removeDebridTransferAsync(providerKind, key, debridId);
                        }, deploy_));
                    return;
                }
                import.fileSelection = plan.fileSelection;
                import.packageCount = plan.packageCount;
                import.mode = plan.mode;
                const SkippedExtraNotice extrasNotice = plan.extras;
                std::string id;
                std::string importError;
                if (!manager_->importDebrid(import, id, importError)) {
                    removeDebridTransferAsync(providerKind, key, debridId);
                    operationMessage_ = importError;
                    brls::Application::notify(importError);
                } else {
                    if (!notifyExtrasSkipped(extrasNotice))
                        statusLabel_->setText(tr("pipensx/debrid/queued"));
                    if (onChange_)
                        onChange_();
                }
                refreshButtons();
                };
                if (!cardOneTapUsesPortInstall(layoutPreview) &&
                    !entry.isAutorun() && torrentHasInstallableExtras(layoutPreview)) {
                    showExtrasChoice(std::move(queueResolved),
                        [this, import, layoutPreview, providerKind, key, debridId]() mutable {
                            brls::Application::pushActivity(new TorrentSelectionActivity(
                                manager_, "", std::move(layoutPreview), TransferMode::StreamInstall,
                                settings_->get().streamSelection, {}, import,
                                [providerKind, key, debridId] {
                                    removeDebridTransferAsync(providerKind, key, debridId);
                                }));
                        }, [providerKind, key, debridId] {
                            removeDebridTransferAsync(providerKind, key, debridId);
                        });
                } else {
                    queueResolved();
                }
            });
        });
    }

    void showExtrasChoice(std::function<void()> regular,
                          std::function<void()> picker,
                          std::function<void()> cancel) {
        setBusy(true);
        auto alive = alive_;
        auto resume = [this, alive](std::function<void()> action) {
            return [this, alive, action = std::move(action)] {
                if (!alive->load()) return;
                setBusy(false);
                refreshButtons();
                action();
            };
        };
        openPortInstallDialog(resume(std::move(regular)), resume(std::move(picker)),
            [this, alive, cancel = std::move(cancel)] {
                cancel();
                if (alive->load()) { setBusy(false); refreshButtons(); }
            }, tr("pipensx/port_install/extras_hint"), true,
            tr("pipensx/port_install/extras_warning"),
            tr("pipensx/port_install/regular_install"),
            tr("pipensx/port_install/choose_manually"));
    }

    void finishImport(const std::string& path, bool forcePicker,
                      std::vector<uint8_t> initialPeers, bool extrasAcknowledged = false) {
        pipensx::TorrentPreview preview;
        std::string error;
        if (!DownloadManager::previewTorrent(path, preview, error)) {
            diagnostic_error("catalog", "preview", "error=%s",
                             error.c_str());
            operationMessage_ = error;
            refreshButtons();
            brls::Application::notify(error);
            ::unlink(path.c_str());
            return;
        }

        std::vector<uint8_t> actions = smartInstallMask(preview);
        const auto sized = pipensx::estimateInstallSpace(
            preview, actions, TransferMode::StreamInstall);
        if (!preview.files.empty() && !sized.overflow) {
            installBytes_ = sized.requiredBytes;
            sizeExact_ = true;
            scheduleSizeMeterRefresh();
        }

        // Picker path: the per-file picker owns the temp file and unlinks it
        // on cancel. Each row chooses Skip, Download, or Install directly.
        if (forcePicker) {
            openSelection(path, std::move(preview), std::move(initialPeers));
            return;
        }

        const OneTapPlan plan = planCatalogOneTap(preview, oneTapContext());
        if (!extrasAcknowledged && !entry_.isAutorun() &&
            !cardOneTapUsesPortInstall(preview) && torrentHasInstallableExtras(preview)) {
            showExtrasChoice(
                [this, path, initialPeers] { finishImport(path, false, initialPeers, true); },
                [this, path, preview, initialPeers] { openSelection(path, preview, initialPeers); },
                [path] { ::unlink(path.c_str()); });
            return;
        }
        if (plan.mode == TransferMode::AutoRunInstall) {
            std::string autorunFolder;
            if (entry_.autorun)
                autorunFolder = entry_.autorun->targetFolder;
            std::string id;
            std::string err;
            if (manager_->importTorrentActions(path, plan.fileSelection, id, err,
                                               initialPeers, TransferMode::AutoRunInstall,
                                               autorunFolder)) {
                log_msg("[autorun] queued autorun task %s in folder %s\n",
                        id.c_str(), autorunFolder.c_str());
                if (entry_.autorun) {
                    std::string profileError;
                    auto profile = *entry_.autorun;
                    profile.coverUrl = presentation_.coverUrl;
                    saveAutorunProfile(manager_->rootPath(), id, profile, profileError);
                }
                if (deploy_)
                    deploy_->armAutoCopy(id);
                statusLabel_->setText(tr("pipensx/autorun/queued"));
                brls::Application::notify(tr("pipensx/autorun/queued"));
                if (onChange_)
                    onChange_();
            } else if (catalogLower(err).find("already in the download manager") !=
                       std::string::npos) {
                statusLabel_->setText(tr("pipensx/detail/already_in_downloads"));
                if (onChange_)
                    onChange_();
            } else {
                log_msg("[catalog] autorun import failed from '%s': %s\n",
                        path.c_str(), err.c_str());
                diagnostic_error("catalog", "import", "error=%s", err.c_str());
                operationMessage_ = err;
                brls::Application::notify(err);
            }
            ::unlink(path.c_str());
            refreshButtons();
            return;
        }
        if (plan.outcome == OneTapOutcome::QueuePort) {
            auto pending = std::make_shared<PortImportPending>();
            pending->torrentPath = path;
            pending->preview = std::move(preview);
            pending->peers = std::move(initialPeers);
            beginResolvedPortInstall(pending);
            return;
        }
        if (plan.outcome == OneTapOutcome::NeedsChooser) {
            operationMessage_ = oneTapBlockText(plan.block);
            refreshButtons();
            brls::Application::notify(operationMessage_);
            openSelection(path, std::move(preview), std::move(initialPeers),
                          plan.chooserMode);
            return;
        }

        const bool titleInstalled = installed_ && installed_->contains(titleId_);
        const std::vector<uint8_t>& mask = plan.fileSelection;

        std::string id;
        std::string err;
        const std::string destination = installDestinationLabel(
            settings_ ? installTargetFor(settings_->get().installLocation)
                      : manager_->installTarget());
        if (manager_->importTorrentActions(path, mask, id, err, initialPeers)) {
            log_msg("[catalog] imported torrent %s\n", id.c_str());
            if (!notifyExtrasSkipped(skippedExtraNotice(preview, mask))) {
                if (titleInstalled) {
                    statusLabel_->setText(
                        tr("pipensx/detail/smart_installing_update", destination));
                    brls::Application::notify(
                        tr("pipensx/detail/smart_installing_update", destination));
                } else {
                    statusLabel_->setText(
                        tr("pipensx/detail/smart_installing_base", destination));
                    brls::Application::notify(
                        tr("pipensx/detail/smart_installing_base", destination));
                }
            }
            if (onChange_)
                onChange_();
        } else if (catalogLower(err).find("already in the download manager") !=
                   std::string::npos) {
            statusLabel_->setText(tr("pipensx/detail/already_in_downloads"));
            if (onChange_)
                onChange_();
        } else {
            log_msg("[catalog] import failed from '%s': %s\n",
                    path.c_str(), err.c_str());
            diagnostic_error("catalog", "import", "error=%s", err.c_str());
            operationMessage_ = err;
            refreshButtons();
            brls::Application::notify(err);
        }
        ::unlink(path.c_str());
        refreshButtons();
    }

    void openSelection(const std::string& path,
                       pipensx::TorrentPreview preview,
                       std::vector<uint8_t> initialPeers,
                       TransferMode preferred = TransferMode::StreamInstall) {
        if (entry_.isAutorun())
            preferred = TransferMode::AutoRunInstall;
        StreamSelection selection = settings_
            ? settings_->get().streamSelection : StreamSelection::AllFiles;
        brls::Application::pushActivity(new TorrentSelectionActivity(
            manager_, path, std::move(preview), preferred,
            selection, std::move(initialPeers), {}, {}, deploy_));
    }

    OneTapContext oneTapContext() const {
        OneTapContext context;
        context.titleId = titleId_;
        context.titleInstalled = installed_ && installed_->contains(titleId_);
        context.installedVersion = installedVersionForTitle();
        context.latestVersion = latestVersionForEntry();
        context.installedDlcIds = installedDlcIds();
        context.isAutorun = entry_.isAutorun();
        if (entry_.autorun)
            context.autorunProfile = *entry_.autorun;
        return context;
    }

    std::string oneTapBlockText(OneTapBlock block) const {
        if (block == OneTapBlock::UnidentifiedPackages)
            return tr("pipensx/detail/bundle_choose_files");
        if (block == OneTapBlock::CartridgeOnly)
            return tr("pipensx/detail/cartridge_only");
        if (block == OneTapBlock::NothingSelected &&
            installed_ && installed_->contains(titleId_))
            return tr("pipensx/detail/smart_open_options");
        return tr("pipensx/detail/no_installable");
    }

    std::string installedVersionForTitle() const {
        if (!installed_ || !installed_->contains(titleId_))
            return {};
        for (const InstalledTitle& title : installed_->titles()) {
            if (catalogLower(title.titleId) == catalogLower(titleId_))
                return title.version;
        }
        return {};
    }

    bool installedTitleCopy(InstalledTitle& out) const {
        if (!installed_ || titleId_.empty())
            return false;
        for (const InstalledTitle& title : installed_->titles()) {
            if (catalogLower(title.titleId) == catalogLower(titleId_)) {
                out = title;
                return true;
            }
        }
        return false;
    }

    // B6: "5.0.0 (1.26.30)" — decimal patch plus NACP display_version.
    std::string installedVersionLabel() const {
        InstalledTitle current;
        if (!installedTitleCopy(current))
            return {};
        return formatInstalledVersionLabel(current.version,
                                            current.displayVersion);
    }

    bool hasModsForTitle() const {
        InstalledTitle current;
        return installedTitleCopy(current) && current.hasLayeredFsMods;
    }

    // B6: mods row for the facts table; empty (hidden) when none.
    std::string modsFact() const {
        if (!hasModsForTitle() || titleId_.empty())
            return {};
        return tr("pipensx/detail/mods_detected", titleId_);
    }

    // B6: append the actionable hint to a raw backend error. Signature-
    // patch markers win (they name the fix); installed mods come next
    // (BOTW-style stale-mod break); firmware/system wording falls back to
    // the HOS hint. Unknown errors pass through unchanged.
    std::string withUpdateFailureHint(const std::string& error) const {
        if (error.empty() || titleId_.empty())
            return error;
        const std::string lower = catalogLower(error);
        const bool sigPatch =
            lower.find("0291") != std::string::npos ||
            lower.find("0x291") != std::string::npos ||
            lower.find("sys-patch") != std::string::npos ||
            lower.find("syspatch") != std::string::npos ||
            lower.find("sigpatch") != std::string::npos ||
            lower.find("sig-patch") != std::string::npos ||
            lower.find("signature patch") != std::string::npos;
        if (sigPatch)
            return error + "\n" +
                   tr("pipensx/detail/update_failure_sigpatch_hint");
        if (hasModsForTitle())
            return error + "\n" +
                   tr("pipensx/detail/update_failure_mods_hint", titleId_);
        if (lower.find("firmware") != std::string::npos ||
            lower.find("system update") != std::string::npos ||
            lower.find("required_system") != std::string::npos ||
            lower.find("required system") != std::string::npos)
            return error + "\n" +
                   tr("pipensx/detail/update_failure_hos_hint");
        return error;
    }

    std::string latestVersionForEntry() const {
        if (entry_.hasTitlesField || !entry_.titles.empty()) {
            for (const auto& title : entry_.titles)
                if (catalogLower(title.titleId) == catalogLower(titleId_))
                    return title.updateVersion;
            return {};
        }
        const std::string catalogVersion = catalogUpdateVersion(entry_, titleId_);
        if (!catalogVersion.empty())
            return catalogVersion;
        const GameMetadata* metadata =
            metadata_->findByInfoHash(entry_.infoHash, titleId_);
        return metadata ? metadata->latestVersion : std::string();
    }

    std::vector<std::string> installedDlcIds() const {
        return installed_ ? installed_->dlcTitleIds() : std::vector<std::string>();
    }

    // Sets the status line and the one-tap toast. Returns false when nothing
    // extra was skipped, so the caller can show the ordinary queued text.
    bool notifyExtrasSkipped(SkippedExtraNotice notice) {
        if (notice == SkippedExtraNotice::None)
            return false;
        statusLabel_->setText(tr("pipensx/detail/installing_extras_skipped"));
        brls::Application::notify(tr(
            notice == SkippedExtraNotice::Installable
                ? "pipensx/detail/installing_extras_skipped_hint"
                : "pipensx/detail/installing_extras_skipped_junk_hint"));
        return true;
    }

    std::vector<uint8_t> smartInstallMask(
        const TorrentPreview& preview) const {
        const bool titleInstalled = installed_ && installed_->contains(titleId_);
        return selectSmartInstallFiles(preview, titleInstalled,
                                       installedVersionForTitle(),
                                       latestVersionForEntry(), titleId_,
                                       installedDlcIds());
    }

    CatalogEntry entry_;
    CatalogPresentation presentation_;
    std::string lastFailure_;
    DownloadManager* manager_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    FavoritesService* favorites_ = nullptr;
    SwitchDeployService* deploy_ = nullptr;
    AutorunService* autorun_ = nullptr;
    std::string titleId_;
    std::string playersFact_;
    std::string operationMessage_;
    FailureCallback onFailure_;
    ChangeCallback onChange_;
    CloseCallback onClose_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    brls::AppletFrame* frame_ = nullptr;
    InstallButton* primary_ = nullptr;
    brls::Button* secondary_ = nullptr;
    brls::Button* favorite_ = nullptr;
    StorageMeter* sizeMeter_ = nullptr;
    brls::Label* statusLabel_ = nullptr;
    uint64_t installBytes_ = 0;
    bool sizeExact_ = false;
    int storageTick_ = 0;
    bool storageQueryInFlight_ = false;
    bool storageRefreshPending_ = false;
    bool storageMeterTargetValid_ = false;
    pipensx::install::InstallStorageTarget storageQueryTarget_ =
        pipensx::install::InstallStorageTarget::SdCard;
    pipensx::install::InstallStorageTarget storageMeterTarget_ =
        pipensx::install::InstallStorageTarget::SdCard;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> cache_;
    bool busy_ = false;
    bool autoInstall_ = false;
};

}  // namespace pipensx::ui
