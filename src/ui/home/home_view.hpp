#pragma once

#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_presentation.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/favorites_service.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/switch_deploy.hpp"
#include "ui/catalog/catalog_helpers.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/detail/game_detail.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

inline std::string homeBlurb(const std::string& value) {
    constexpr size_t kMax = 420;
    if (value.size() <= kMax)
        return value;
    size_t cut = value.rfind(' ', kMax);
    if (cut == std::string::npos || cut < kMax / 2)
        cut = kMax;
    while (cut > 0 &&
           (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80)
        --cut;
    return value.substr(0, cut) + "...";
}

class HomeBanner : public brls::Box {
public:
    using Activate = std::function<void()>;

    HomeBanner() : brls::Box(brls::Axis::ROW) {
        setFocusable(true);
        setGrow(1.0f);
        setCornerRadius(theme::kRadiusLarge);
        setClipsToBounds(true);
        setBackgroundColor(theme::surface());

        art_ = new brls::Box();
        art_->setGrow(1.65f);
        art_->setMinWidth(0);
        art_->setClipsToBounds(true);
        art_->setAlignItems(brls::AlignItems::CENTER);
        art_->setJustifyContent(brls::JustifyContent::CENTER);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(theme::kFontTitle);
        placeholder_->setTextColor(theme::textSecondary());
        art_->addView(placeholder_);
        image_ = new AsyncRgbaImage();
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setWidthPercentage(100);
        image_->setHeightPercentage(100);
        image_->setScalingType(brls::ImageScalingType::FILL);
        art_->addView(image_);
        addView(art_);

        auto* copy = new brls::Box(brls::Axis::COLUMN);
        copy->setWidth(400);
        copy->setShrink(0.0f);
        copy->setBackgroundColor(theme::panel());
        copy->setPadding(3 * theme::kSpacingUnit, 3 * theme::kSpacingUnit,
                         3 * theme::kSpacingUnit, 3 * theme::kSpacingUnit);
        kicker_ = new brls::Label();
        kicker_->setFontSize(theme::kFontCaption);
        kicker_->setTextColor(theme::accent());
        title_ = new brls::Label();
        title_->setSingleLine(true);
        title_->setAutoAnimate(false);
        title_->setFontSize(theme::kFontHeading);
        title_->setMarginTop(6);
        meta_ = new brls::Label();
        meta_->setSingleLine(true);
        meta_->setAutoAnimate(false);
        meta_->setFontSize(theme::kFontCaption);
        meta_->setTextColor(theme::textSecondary());
        meta_->setMarginTop(6);
        description_ = new brls::Label();
        description_->setFontSize(theme::kFontSmall);
        description_->setTextColor(theme::textSecondary());
        description_->setMarginTop(12);
        description_->setGrow(1.0f);
        copy->addView(kicker_);
        copy->addView(title_);
        copy->addView(meta_);
        copy->addView(description_);
        addView(copy);

        registerAction("", brls::BUTTON_A,
                       [this](brls::View*) {
                           if (onActivate_)
                               onActivate_();
                           return true;
                       },
                       /*hidden=*/true);
        addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    void setSlot(const CatalogEntry& entry, const GameMetadata* meta,
                 const std::string& kicker, GameMetadataService* service,
                 Activate onActivate) {
        infoHash_ = entry.infoHash;
        onActivate_ = std::move(onActivate);
        CatalogPresentation presentation =
            resolveCatalogPresentation(entry, meta, catalogTextPreference());
        setTextIfChanged(kicker_, kicker);
        setTextIfChanged(title_, presentation.title);
        std::string metaLine = presentation.genre;
        if (entry.size) {
            if (!metaLine.empty())
                metaLine += " · ";
            metaLine += formatBytes(entry.size);
        }
        setTextIfChanged(meta_, metaLine);
        setTextIfChanged(description_, homeBlurb(presentation.description));
        setTextIfChanged(placeholder_, placeholderLetter(presentation.title));
        const std::string url = catalogFeaturedImageUrl(entry, meta);
        setArtworkUrl(image_, service, url, currentUrl_, imageState_,
                      GameMetadataService::kImageDimFull);
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        title_->setAnimated(true);
    }

    void onFocusLost() override {
        brls::Box::onFocusLost();
        title_->setAnimated(false);
    }

    const std::string& infoHash() const { return infoHash_; }

private:
    brls::Box* art_ = nullptr;
    brls::Label* placeholder_ = nullptr;
    AsyncRgbaImage* image_ = nullptr;
    brls::Label* kicker_ = nullptr;
    brls::Label* title_ = nullptr;
    brls::Label* meta_ = nullptr;
    brls::Label* description_ = nullptr;
    std::string currentUrl_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
    std::string infoHash_;
    Activate onActivate_;
};

class HomeView : public brls::Box {
public:
    HomeView(DownloadManager* manager, CatalogService* catalog,
             GameMetadataService* metadata, InstalledTitleService* installed,
             AppSettings* settings, FavoritesService* favorites,
             SwitchDeployService* deploy, std::function<void()> openSearch)
        : brls::Box(brls::Axis::COLUMN), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed), settings_(settings),
          favorites_(favorites), deploy_(deploy),
          openSearch_(std::move(openSearch)) {
        setGrow(1);
        setPadding(10, 20, 8, 20);

        hero_ = new HomeBanner();
        hero_->registerAction("", brls::BUTTON_LEFT,
                              [this](brls::View*) {
                                  bumpHero(-1);
                                  return true;
                              },
                              /*hidden=*/true, /*allowRepeating=*/true);
        hero_->registerAction("", brls::BUTTON_RIGHT,
                              [this](brls::View*) {
                                  bumpHero(1);
                                  return true;
                              },
                              /*hidden=*/true, /*allowRepeating=*/true);
        addView(hero_);

        dots_ = new brls::Box(brls::Axis::ROW);
        dots_->setFocusable(false);
        dots_->setHeight(18);
        dots_->setShrink(0.0f);
        dots_->setAlignItems(brls::AlignItems::CENTER);
        dots_->setJustifyContent(brls::JustifyContent::CENTER);
        dots_->setMarginTop(6);
        addView(dots_);

        registerAction(tr("pipensx/common/search"), brls::BUTTON_Y,
                       [this](brls::View*) {
                           if (openSearch_)
                               openSearch_();
                           return true;
                       });
        if (favorites_) {
            registerAction(tr("pipensx/catalog/action_favorite"),
                           brls::BUTTON_RT, [this](brls::View*) {
                toggleFocusedFavorite();
                return true;
            });
        }

        rebuild();
        timer_.setCallback([this] { onTick(); });
        timer_.start(kHeroMs);
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        visible_ = true;
        rebuild();
        if (autoplay_)
            timer_.start(kHeroMs);
    }

    void willDisappear(bool resetState) override {
        visible_ = false;
        timer_.stop();
        brls::Box::willDisappear(resetState);
    }

    void freezeHero() {
        autoplay_ = false;
        timer_.stop();
    }

    ~HomeView() override { timer_.stop(); }

private:
    static constexpr int kHeroMs = 5000;
    static constexpr size_t kMaxSlots = 6;

    struct Slot {
        CatalogEntry entry;
        std::string kicker;
    };

    static std::vector<int> newestOrder(const std::vector<CatalogEntry>& all,
                                        const std::vector<int>& visible) {
        std::vector<int> order(visible.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](int left, int right) {
                             return all[static_cast<size_t>(
                                        visible[static_cast<size_t>(left)])]
                                        .publishedAt >
                                    all[static_cast<size_t>(
                                        visible[static_cast<size_t>(right)])]
                                        .publishedAt;
                         });
        return order;
    }

    void rebuild() {
        slots_.clear();
        if (!catalog_)
            return;
        auto snapshot = catalog_->sharedEntries();
        const std::vector<CatalogEntry>& all = *snapshot;
        std::vector<int> games;
        std::vector<int> ports;
        games.reserve(all.size());
        ports.reserve(all.size());
        for (size_t i = 0; i < all.size(); ++i) {
            const CatalogEntry& entry = all[i];
            if (entry.isHiddenByDefault())
                continue;
            const GameMetadata* meta =
                metadata_ ? metadata_->findByInfoHash(entry.infoHash) : nullptr;
            if (catalogEntryIsGame(entry, meta))
                games.push_back(static_cast<int>(i));
            else
                ports.push_back(static_cast<int>(i));
        }
        bool fallback = false;
        const std::vector<int> gamesPopular =
            catalogPopularityOrder(all, games, fallback);
        const std::vector<int> portsPopular =
            catalogPopularityOrder(all, ports, fallback);
        const std::vector<int> gamesNew = newestOrder(all, games);
        const std::vector<int> portsNew = newestOrder(all, ports);
        std::unordered_set<std::string> used;
        auto pushSlot = [&](const std::vector<int>& visible, int visIndex,
                            const std::string& kicker, bool requireArt) {
            if (slots_.size() >= kMaxSlots)
                return;
            if (visIndex < 0 ||
                static_cast<size_t>(visIndex) >= visible.size())
                return;
            const CatalogEntry& entry =
                all[static_cast<size_t>(visible[static_cast<size_t>(visIndex)])];
            const GameMetadata* meta =
                metadata_ ? metadata_->findByInfoHash(entry.infoHash) : nullptr;
            if (requireArt && catalogFeaturedImageUrl(entry, meta).empty())
                return;
            std::string key = !entry.titleId.empty()
                ? std::string("t:") + entry.titleId
                : (meta && !meta->titleId.empty()
                       ? std::string("t:") + meta->titleId
                       : std::string("h:") + catalogLower(entry.infoHash));
            if (!used.insert(key).second)
                return;
            slots_.push_back({entry, kicker});
        };
        auto fill = [&](const std::vector<int>& visible,
                        const std::vector<int>& order,
                        const std::string& kicker, size_t want,
                        bool requireArt) {
            const size_t before = slots_.size();
            for (int visIndex : order) {
                if (slots_.size() - before >= want)
                    break;
                pushSlot(visible, visIndex, kicker, requireArt);
            }
        };
        const std::string popular = tr("pipensx/catalog/shelf_popular");
        const std::string newest = tr("pipensx/catalog/shelf_new");
        const std::string portKicker = tr("pipensx/nav/ports");
        fill(games, gamesPopular, popular, 2, true);
        fill(games, gamesNew, newest, 2, true);
        fill(ports, portsPopular, portKicker, 1, true);
        fill(ports, portsNew, portKicker, 1, true);
        fill(games, gamesPopular, popular, kMaxSlots, false);
        fill(games, gamesNew, newest, kMaxSlots, false);
        fill(ports, portsPopular, portKicker, kMaxSlots, false);
        fill(ports, portsNew, portKicker, kMaxSlots, false);
        if (heroIndex_ >= static_cast<int>(slots_.size()))
            heroIndex_ = 0;
        paint();
        const bool empty = slots_.empty();
        hero_->setVisibility(empty ? brls::Visibility::GONE
                                   : brls::Visibility::VISIBLE);
        dots_->setVisibility(empty ? brls::Visibility::GONE
                                   : brls::Visibility::VISIBLE);
        if (empty) {
            if (!emptyState_) {
                emptyState_ = new EmptyStateView();
                addView(emptyState_);
            }
            emptyState_->setContent(
                tr("pipensx/catalog/empty_title"),
                tr("pipensx/catalog/empty_body"), "", nullptr);
            emptyState_->setVisibility(brls::Visibility::VISIBLE);
        } else if (emptyState_) {
            emptyState_->setVisibility(brls::Visibility::GONE);
        }
    }

    void paint() {
        if (slots_.empty())
            return;
        const Slot& slot = slots_[static_cast<size_t>(heroIndex_)];
        const GameMetadata* meta =
            metadata_ ? metadata_->findByInfoHash(slot.entry.infoHash)
                      : nullptr;
        const std::string hash = slot.entry.infoHash;
        hero_->setSlot(slot.entry, meta, slot.kicker, metadata_,
                       [this, hash] { openDetail(hash); });
        rebuildDots();
    }

    void rebuildDots() {
        const int n = static_cast<int>(slots_.size());
        while (static_cast<int>(dotViews_.size()) < n) {
            auto* dot = new brls::Box();
            dot->setFocusable(false);
            dot->setWidth(8);
            dot->setHeight(8);
            dot->setCornerRadius(4);
            dot->setMarginLeft(4);
            dot->setMarginRight(4);
            dots_->addView(dot);
            dotViews_.push_back(dot);
        }
        for (size_t i = 0; i < dotViews_.size(); ++i) {
            const bool on = static_cast<int>(i) == heroIndex_;
            dotViews_[i]->setWidth(on ? 18.0f : 8.0f);
            dotViews_[i]->setBackgroundColor(
                on ? theme::textPrimary() : theme::textTertiary());
            dotViews_[i]->setVisibility(
                static_cast<int>(i) < n ? brls::Visibility::VISIBLE
                                        : brls::Visibility::GONE);
        }
    }

    void bumpHero(int delta) {
        if (slots_.size() < 2)
            return;
        const int n = static_cast<int>(slots_.size());
        heroIndex_ = (heroIndex_ + delta % n + n) % n;
        paint();
    }

    void onTick() {
        if (!visible_ || !autoplay_ || slots_.size() < 2)
            return;
        if (brls::Application::getCurrentFocus() == hero_)
            return;
        bumpHero(1);
    }

    void openDetail(const std::string& hash) {
        const CatalogEntry* picked = nullptr;
        for (const Slot& slot : slots_) {
            if (slot.entry.infoHash == hash) {
                picked = &slot.entry;
                break;
            }
        }
        if (!picked)
            return;
        CatalogEntry entry = *picked;
        const GameMetadata* meta =
            metadata_ ? metadata_->findByInfoHash(entry.infoHash) : nullptr;
        const bool port = catalogEntryIsPort(entry, meta);
        brls::Application::pushActivity(new GameDetailActivity(
            std::move(entry), "", manager_, metadata_, installed_, settings_,
            [](const std::string&, const std::string&) {},
            [this] { rebuild(); }, [this] { rebuild(); }, favorites_, deploy_,
            false, port));
    }

    void toggleFocusedFavorite() {
        if (!favorites_)
            return;
        auto* banner =
            dynamic_cast<HomeBanner*>(brls::Application::getCurrentFocus());
        if (!banner || banner->infoHash().empty())
            return;
        const Slot* slot = nullptr;
        for (const Slot& candidate : slots_) {
            if (candidate.entry.infoHash == banner->infoHash()) {
                slot = &candidate;
                break;
            }
        }
        if (!slot)
            return;
        if (!favorites_->contains(slot->entry.infoHash) &&
            favorites_->items().size() >= FavoritesService::kMaxFavorites) {
            brls::Application::notify(
                tr("pipensx/catalog/favorites_full",
                   FavoritesService::kMaxFavorites));
            return;
        }
        const GameMetadata* meta =
            metadata_ ? metadata_->findByInfoHash(slot->entry.infoHash)
                      : nullptr;
        CatalogPresentation presentation = resolveCatalogPresentation(
            slot->entry, meta, catalogTextPreference());
        std::string error;
        favorites_->toggle(slot->entry.infoHash, presentation.title, error);
        if (!error.empty())
            brls::Application::notify(
                tr("pipensx/catalog/favorites_failed", error));
    }

    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    FavoritesService* favorites_;
    SwitchDeployService* deploy_;
    std::function<void()> openSearch_;
    HomeBanner* hero_ = nullptr;
    brls::Box* dots_ = nullptr;
    std::vector<brls::Box*> dotViews_;
    EmptyStateView* emptyState_ = nullptr;
    std::vector<Slot> slots_;
    int heroIndex_ = 0;
    bool visible_ = false;
    bool autoplay_ = true;
    brls::RepeatingTimer timer_;
};

}  // namespace pipensx::ui
