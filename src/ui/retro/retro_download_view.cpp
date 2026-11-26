#include "retro_download_view.hpp"
#include "app/add_release.hpp"
#include "app/install_space.hpp"
#include "app/magnet_resolver.hpp"
#include "app/retro_forwarder.hpp"
#include "app/task_files.hpp"
#include "app/torrent_metainfo_fetch.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/catalog_controls.hpp"
#include "ui/common/description_text.hpp"
#include "ui/common/detail_fact_row.hpp"
#include "ui/common/detail_frame.hpp"
#include "ui/common/message_cells.hpp"
#include "ui/common/qr_view.hpp"
#include "ui/common/smooth_recycler.hpp"
#include "ui/common/stick_scrolling_frame.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/detail/screenshot_viewer.hpp"
#include "ui/detail/torrent_selection.hpp"
#include "ui/downloads/details_activity.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace pipensx::ui {
namespace {

class RetroActionButton : public theme::Button {
public:
    void setState(brls::ButtonState state, bool keepFocusableWhenDisabled = false) {
        enabled_ = state == brls::ButtonState::ENABLED;
        brls::Button::setState(state);
        setFocusable(enabled_ || keepFocusableWhenDisabled);
        setAlpha(enabled_ ? 1.0f : 0.65f);
    }
    bool enabled() const { return enabled_; }

private:
    bool enabled_ = true;
};

brls::Label* makeLabel(brls::Box* box, const std::string& value, float font = theme::kFontBody,
                       NVGcolor color = theme::textPrimary()) {
    auto* label = new brls::Label();
    label->setText(value);
    label->setFontSize(font);
    label->setTextColor(color);
    label->setSingleLine(false);
    label->setMarginBottom(10);
    box->addView(label);
    return label;
}

RetroActionButton* makeButton(brls::Box* box, const std::string& value,
                              std::function<void()> callback, float height = 50.0f) {
    auto* result = new RetroActionButton();
    result->setText(value);
    result->setStyle(&brls::BUTTONSTYLE_DEFAULT);
    result->setFontSize(theme::kFontSmall);
    result->setHeight(height);
    result->setShrink(0);
    result->setMarginBottom(10);
    result->registerClickAction([result, callback = std::move(callback)](brls::View*) {
        if (result->enabled())
            callback();
        return true;
    });
    box->addView(result);
    return result;
}

DetailFrame* pushDetailActivity(brls::View* body, const std::string& title) {
    auto* frame = new DetailFrame(body);
    frame->setTitle(title);
    brls::Application::pushActivity(new brls::Activity(frame));
    return frame;
}

std::string baseNameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stemOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return name;
    return name.substr(0, dot);
}

std::string emulatorDisplayName(const std::string& emuId) {
    if (emuId.empty())
        return tr("pipensx/retro/default_emulator");
    if (isCustomEmulatorPath(emuId))
        return baseNameOf(emuId) + " (" + tr("pipensx/retro/custom_nro") + ")";
    if (const auto* profile = findRetroEmulatorProfile(emuId))
        return profile->displayName;
    return emuId;
}

std::string emulatorLabelWithRuntimeTag(const RetroEmulatorProfile& profile) {
    std::string out = profile.displayName;
    if (out.find("RetroArch") == std::string::npos)
        out += profile.isRetroArchCore
                   ? " [RetroArch]"
                   : (" [" + tr("pipensx/retro/runtime_standalone") + "]");
    return out;
}

std::vector<RetroEmulatorPackage> loadCachedEmulatorPackages(const std::string& appRoot) {
    if (appRoot.empty())
        return {};
    std::map<std::string, RetroStats> unusedStats;
    std::vector<RetroEmulatorPackage> emulators;
    RetroSource unusedSource;
    std::string unusedErr;
    RetroCatalog(appRoot).loadShared(unusedStats, emulators, unusedSource, unusedErr);
    return emulators;
}

std::string localizeRetroError(const std::string& error) {
    if (error.empty())
        return {};
    struct Mapping {
        const char* exact;
        const char* key;
    };
    static constexpr Mapping kExact[] = {
        {"No matching ROM files found in folder.", "pipensx/retro/err_import_empty"},
        {"Import folder does not exist.", "pipensx/retro/err_import_missing"},
        {"Invalid ROM import folder.", "pipensx/retro/err_import_invalid"},
        {"Multi-volume ROM archives are not supported.", "pipensx/retro/err_multivolume"},
        {"Multi-volume RAR5 archives are not supported.", "pipensx/retro/err_multivolume"},
        {"Encrypted or multi-volume RAR archives are not supported.",
         "pipensx/retro/err_encrypted_archive"},
        {"Encrypted or multi-volume RAR members are not supported.",
         "pipensx/retro/err_encrypted_archive"},
        {"Password-protected ROM archives are not supported.",
         "pipensx/retro/err_encrypted_archive"},
        {"Password-protected RAR5 archives are not supported.",
         "pipensx/retro/err_encrypted_archive"},
        {"Encrypted emulator archives are not supported.",
         "pipensx/retro/err_encrypted_archive"},
        {"Insufficient free space for emulator installation.", "pipensx/retro/err_no_space"},
        {"ROM preparation exceeds its path, file count, size or free-space limit.",
         "pipensx/retro/err_no_space"},
        {"Library ROM is missing or invalid.", "pipensx/retro/err_rom_missing"},
        {"Prepared ROM receipt is missing or changed.", "pipensx/retro/err_rom_missing"},
        {"Selected ROM is missing or invalid.", "pipensx/retro/err_rom_missing"},
        {"Selected download file is missing or unsafe.", "pipensx/retro/err_download_files_missing"},
        {"Custom NRO not found.", "pipensx/retro/err_custom_nro_missing"},
        {"Unsupported ROM extension for selected emulator.", "pipensx/retro/err_unsupported_rom_ext"},
        {"Invalid emulator NRO launch path.", "pipensx/retro/err_invalid_launch_path"},
        {"Invalid ROM launch path.", "pipensx/retro/err_invalid_launch_path"},
    };
    if (error == "No valid GBA ROMs found; downloaded files have been kept.")
        return tr("pipensx/retro/err_no_valid_roms", "GBA");
    for (const auto& m : kExact) {
        if (error == m.exact)
            return tr(m.key);
    }
    constexpr std::string_view kNoValidPrefix = "No valid ROMs found for ";
    constexpr std::string_view kNoValidSuffix = "; downloaded files have been kept.";
    if (error.size() > kNoValidPrefix.size() + kNoValidSuffix.size() &&
        std::string_view(error).substr(0, kNoValidPrefix.size()) == kNoValidPrefix &&
        std::string_view(error).substr(error.size() - kNoValidSuffix.size()) == kNoValidSuffix) {
        return tr("pipensx/retro/err_no_valid_roms",
                  error.substr(kNoValidPrefix.size(),
                               error.size() - kNoValidPrefix.size() - kNoValidSuffix.size()));
    }
    constexpr std::string_view kNotInstalledSuffix = " is not installed.";
    if (error.size() > kNotInstalledSuffix.size() &&
        std::string_view(error).substr(error.size() - kNotInstalledSuffix.size()) ==
            kNotInstalledSuffix) {
        return tr("pipensx/retro/err_emulator_not_installed",
                  error.substr(0, error.size() - kNotInstalledSuffix.size()));
    }
    return error;
}

std::string consoleShortBadge(const std::string& consoleId) {
    if (const auto* c = findRetroConsole(consoleId))
        return c->shortBadge;
    return consoleId;
}

std::string consoleFullName(const std::string& consoleId) {
    if (const auto* c = findRetroConsole(consoleId))
        return c->defaultName;
    return consoleId;
}

RetroBiosStatus configuredBiosStatus(const RetroSettings& settings, const std::string& platform,
                                     const std::string& emulator) {
    const auto dir = settings.biosDirs.find(platform);
    const auto* profile = findRetroEmulatorProfile(emulator);
    const bool useOverride = profile && profile->isRetroArchCore &&
        settings.emulatorFor(platform) == emulator && dir != settings.biosDirs.end();
    return checkBiosStatus("sdmc:", platform, emulator, useOverride ? dir->second : std::string{});
}

std::string formatBiosDialogBody(const RetroBiosStatus& bios) {
    const std::string title = tr(bios.required ? "pipensx/retro/bios_required_title"
                                               : "pipensx/retro/bios_optional_title");
    const std::string hint = bios.hintKey.empty()
        ? tr("pipensx/retro/bios_generic", bios.expectedPath)
        : brls::getStr(bios.hintKey.c_str(), bios.expectedPath);
    return title + "\n\n" + hint;
}

std::string gameDisplayTitle(const RetroRomOperation& entry, const RetroRomGame& game) {
    if (entry.games.size() == 1 && !entry.imported() && !entry.title.empty())
        return entry.title;
    const std::string fileStem = stemOf(baseNameOf(game.path));
    if (!fileStem.empty() && fileStem != ".gba")
        return fileStem;
    return game.title;
}

void pushLibraryGameDetail(RetroRomOperation entry, RetroRomGame game,
                           std::shared_ptr<RetroRomStore> store,
                           std::shared_ptr<RetroEmulatorInstaller> installer,
                           DownloadManager* manager, GameMetadataService* metadata,
                           AppSettings* settings, std::function<void()> onChanged);

std::string resolveEffectiveEmulatorId(const RetroSettings& settings,
                                       const RetroRomOperation& entry,
                                       const RetroRomGame& game) {
    if (!game.emulator.empty() && isValidEmulatorSelection(entry.platform, game.emulator))
        return game.emulator;
    if (!entry.emulator.empty() && isValidEmulatorSelection(entry.platform, entry.emulator))
        return entry.emulator;
    return settings.emulatorFor(entry.platform);
}

std::shared_ptr<RetroForwarderService> makeForwarderService(
    std::string appRoot,
    std::shared_ptr<RetroRomStore> store,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    GameMetadataService* metadata) {
    return std::make_shared<RetroForwarderService>(
        appRoot,
        [store, installer, metadata](const std::string& id, RetroForwarderTarget& target,
                                     std::string& error) {
            RetroRomGame game;
            RetroRomOperation entry;
            std::string romPath, nro, args, unusedErr;
            if (!store->resolveLibraryGame(id, romPath, error, &game, &entry))
                return false;
            RetroSettings retroSettings;
            store->loadSettings(retroSettings, unusedErr);
            const std::string emuId = resolveEffectiveEmulatorId(retroSettings, entry, game);
            if (!installer->detect(retroSettings.detectionFor(entry.platform, emuId), nro, error) ||
                !store->prepareLaunchArguments(retroSettings, entry.platform, emuId, nro, romPath, args, error))
                return false;
            target.title = gameDisplayTitle(entry, game);
            target.nro = nro;
            target.arguments = args;
            if (metadata && !entry.coverUrl.empty()) {
                std::string imgErr;
                metadata->loadImage(entry.coverUrl, target.iconJpeg, imgErr);
            }
            return true;
        },
        retroForwarderPlatform(appRoot));
}

brls::Box* makeBiosBanner(const RetroBiosStatus& bios) {
    auto* card = new brls::Box(brls::Axis::COLUMN);
    card->setPadding(10, 12, 10, 12);
    card->setCornerRadius(theme::kRadiusSmall);
    card->setBackgroundColor(theme::panel());
    card->setLineColor(bios.required ? theme::warning() : theme::accent());
    card->setLineLeft(3.0f);
    card->setMarginTop(8);
    card->setMarginBottom(8);
    card->setShrink(0);

    auto* heading = new brls::Label();
    heading->setText(tr(bios.required ? "pipensx/retro/bios_required_title"
                                      : "pipensx/retro/bios_optional_title"));
    heading->setFontSize(theme::kFontCaption);
    heading->setTextColor(bios.required ? theme::color(theme::Tok::ChipWarning) : theme::textPrimary());
    heading->setSingleLine(false);
    heading->setMarginBottom(4);
    card->addView(heading);

    auto* detail = new brls::Label();
    const std::string msg =
        bios.hintKey.empty()
            ? tr("pipensx/retro/bios_generic", bios.expectedPath)
            : brls::getStr(bios.hintKey.c_str(), bios.expectedPath);
    detail->setText(msg);
    detail->setFontSize(theme::kFontCaption);
    detail->setTextColor(theme::textSecondary());
    detail->setSingleLine(false);
    card->addView(detail);
    return card;
}

void promptCustomNroPath(const std::string& initial, std::function<void(const std::string&)> onChosen) {
    const std::string start = initial.empty() ? "sdmc:/switch/" : initial;
    brls::Application::getImeManager()->openForText(
        [onChosen = std::move(onChosen)](std::string value) {
            while (!value.empty() && value.back() == ' ')
                value.pop_back();
            if (!isCustomEmulatorPath(value)) {
                brls::Application::notify(tr("pipensx/retro/invalid_custom_nro"));
                return;
            }
            onChosen(value);
        },
        tr("pipensx/retro/custom_nro_prompt"), "", 512, start, brls::KEYBOARD_DISABLE_NONE);
}

// ============================================================================
// Game picker for multi-ROM releases (virtualized)
// ============================================================================

class PickerCell : public brls::RecyclerCell {
public:
    PickerCell() {
        setFocusable(true);
        setHeight(64);
        setPadding(8, 24, 8, 24);
        setCornerRadius(theme::kRadiusSmall);
        setAlignItems(brls::AlignItems::CENTER);
        setAxis(brls::Axis::ROW);

        check_ = new brls::Label();
        check_->setFontSize(theme::kFontBody);
        check_->setWidth(44);
        check_->setShrink(0);
        check_->setSingleLine(true);
        addView(check_);

        auto* textCol = new brls::Box(brls::Axis::COLUMN);
        textCol->setGrow(1);
        textCol->setShrink(1);
        textCol->setMinWidth(0);

        title_ = new brls::Label();
        title_->setFontSize(theme::kFontBody);
        title_->setSingleLine(true);
        textCol->addView(title_);

        sub_ = new brls::Label();
        sub_->setFontSize(theme::kFontCaption);
        sub_->setTextColor(theme::textSecondary());
        sub_->setSingleLine(true);
        sub_->setMarginTop(2);
        textCol->addView(sub_);
        addView(textCol);
    }

    void bind(const RetroRomGame& game) {
        check_->setText(game.selected ? "[x]" : "[ ]");
        check_->setTextColor(game.selected ? theme::accent() : theme::textSecondary());
        const std::string stem = stemOf(baseNameOf(game.path));
        std::string display = (!stem.empty() && stem != ".gba") ? stem : game.title;
        if (!game.title.empty() && catalogFoldForSearch(display) != catalogFoldForSearch(game.title))
            display += " (" + game.title + ")";
        title_->setText(display);
        std::string sub = game.path;
        if (game.size > 0)
            sub += " · " + formatBytes(game.size);
        sub_->setText(sub);
    }

private:
    brls::Label *check_ = nullptr, *title_ = nullptr, *sub_ = nullptr;
};

class PickerDataSource : public brls::RecyclerDataSource {
public:
    std::vector<RetroRomGame> allGames;
    std::vector<size_t> visibleIndices;
    std::function<void(size_t allIndex)> onToggle;

    int numberOfRows(brls::RecyclerFrame*, int) override {
        return std::max<int>(1, static_cast<int>(visibleIndices.size()));
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath) override {
        return visibleIndices.empty() ? 120.0f : 64.0f;
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        if (visibleIndices.empty()) {
            auto* cell = static_cast<TextMessageCell*>(recycler->dequeueReusableCell("Message"));
            cell->setMessage(tr("pipensx/retro/no_results"));
            return cell;
        }
        auto* cell = static_cast<PickerCell*>(recycler->dequeueReusableCell("Pick"));
        const size_t idx = visibleIndices[static_cast<size_t>(index.row)];
        cell->bind(allGames[idx]);
        return cell;
    }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override {
        if (index.row >= 0 && static_cast<size_t>(index.row) < visibleIndices.size() && onToggle)
            onToggle(visibleIndices[static_cast<size_t>(index.row)]);
    }
};

class RetroGamePickerView : public brls::Box {
public:
    RetroGamePickerView(RetroRomOperation op, std::shared_ptr<RetroRomStore> store,
                        std::function<void(RetroRomOperation)> onSaved)
        : brls::Box(brls::Axis::COLUMN),
          op_(std::move(op)),
          store_(std::move(store)),
          onSaved_(std::move(onSaved)),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        setGrow(1);

        auto* bar = new brls::Box(brls::Axis::ROW);
        bar->setShrink(0);
        bar->setPadding(8, 28, 8, 28);
        bar->setAlignItems(brls::AlignItems::CENTER);

        selectAll_ = catalogChip(tr("pipensx/retro/select_all"), [this] { setAllVisible(true); });
        selectAll_->setMarginLeft(0);
        bar->addView(selectAll_);

        clearAll_ = catalogChip(tr("pipensx/retro/deselect_all"), [this] { setAllVisible(false); });
        bar->addView(clearAll_);

        searchBtn_ = catalogChip(tr("pipensx/common/search"), [this] { promptSearch(); });
        bar->addView(searchBtn_);

        status_ = new brls::Label();
        status_->setFontSize(theme::kFontCaption);
        status_->setTextColor(theme::textSecondary());
        status_->setSingleLine(true);
        status_->setClipsToBounds(true);
        status_->setGrow(1);
        status_->setShrink(1);
        status_->setMinWidth(0);
        status_->setMarginLeft(12);
        status_->setMarginRight(12);
        status_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        bar->addView(status_);

        addBtn_ = new RetroActionButton();
        addBtn_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        addBtn_->setFontSize(theme::kFontSmall);
        addBtn_->setHeight(42);
        addBtn_->setPadding(0, 18, 0, 18);
        addBtn_->setShrink(0);
        addBtn_->registerClickAction([this](brls::View*) {
            if (addBtn_->enabled())
                saveAndAdd();
            return true;
        });
        bar->addView(addBtn_);
        addView(bar);

        recycler_ = new SmoothRecyclerFrame();
        recycler_->setGrow(1);
        recycler_->estimatedRowHeight = 64.0f;
        recycler_->registerCell("Pick", [] { return new PickerCell(); });
        recycler_->registerCell("Message", [] { return new TextMessageCell(); });
        data_ = new PickerDataSource();
        data_->allGames = op_.games;
        data_->onToggle = [this](size_t idx) {
            if (busy_ || idx >= data_->allGames.size())
                return;
            data_->allGames[idx].selected = !data_->allGames[idx].selected;
            refreshSummary();
            recycler_->reloadData();
        };
        recycler_->setDataSource(data_);
        host_ = recyclerHost(recycler_);
        addView(host_);

        registerAction(tr("pipensx/common/search"), brls::BUTTON_X, [this](brls::View*) {
            promptSearch();
            return true;
        });

        rebuildFilter();
    }

    ~RetroGamePickerView() override { *alive_ = false; }

private:
    void rebuildFilter() {
        if (viewContains(host_, brls::Application::getCurrentFocus()))
            brls::Application::giveFocus(selectAll_);
        data_->visibleIndices.clear();
        const std::string q = catalogFoldForSearch(query_);
        for (size_t i = 0; i < data_->allGames.size(); ++i) {
            const auto& g = data_->allGames[i];
            if (q.empty() || catalogFoldForSearch(g.title + " " + g.path).find(q) != std::string::npos)
                data_->visibleIndices.push_back(i);
        }
        recycler_->reloadData();
        refreshSummary();
    }

    void refreshSummary() {
        size_t selectedCount = 0;
        for (const auto& g : data_->allGames) {
            if (g.selected)
                ++selectedCount;
        }
        status_->setText(query_.empty()
                             ? tr("pipensx/retro/prepared", data_->allGames.size(), selectedCount)
                             : query_ + " (" + std::to_string(data_->visibleIndices.size()) + ")");
        addBtn_->setText(tr("pipensx/retro/add_selected_count", selectedCount));
        const bool canAdd = !busy_ && selectedCount > 0;
        if (!canAdd && viewContains(addBtn_, brls::Application::getCurrentFocus()))
            brls::Application::giveFocus(selectAll_);
        addBtn_->setState(canAdd ? brls::ButtonState::ENABLED : brls::ButtonState::DISABLED);
    }

    void setAllVisible(bool value) {
        if (busy_)
            return;
        for (size_t idx : data_->visibleIndices)
            data_->allGames[idx].selected = value;
        refreshSummary();
        recycler_->reloadData();
    }

    void promptSearch() {
        const auto alive = alive_;
        brls::Application::getImeManager()->openForText(
            [this, alive](std::string v) {
                if (!*alive)
                    return;
                query_ = std::move(v);
                rebuildFilter();
            },
            tr("pipensx/common/search"), "", 128, query_, brls::KEYBOARD_DISABLE_NONE);
    }

    void saveAndAdd() {
        if (busy_)
            return;
        std::vector<std::string> selectedIds;
        for (const auto& g : data_->allGames) {
            if (g.selected)
                selectedIds.push_back(g.id);
        }
        if (selectedIds.empty())
            return;
        busy_ = true;
        refreshSummary();
        status_->setText(tr("pipensx/retro/saving_selection"));
        const auto alive = alive_;
        brls::async([this, alive, store = store_, opId = op_.id, selectedIds,
                     games = data_->allGames]() mutable {
            std::string error;
            bool ok = store->select(opId, selectedIds, error) &&
                      store->addSelectedToLibrary(opId, error);
            RetroRomOperation latest;
            if (ok)
                store->load(opId, latest, error);
            brls::sync([this, alive, ok, error, latest = std::move(latest), games]() mutable {
                if (!*alive)
                    return;
                busy_ = false;
                if (!ok) {
                    status_->setText(localizeRetroError(error));
                    refreshSummary();
                    return;
                }
                if (onSaved_)
                    onSaved_(std::move(latest));
                brls::Application::notify(tr("pipensx/retro/library_added"));
                brls::Application::popActivity();
            });
        });
    }

    RetroRomOperation op_;
    std::shared_ptr<RetroRomStore> store_;
    std::function<void(RetroRomOperation)> onSaved_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::string query_;
    bool busy_ = false;
    theme::Button *selectAll_ = nullptr, *clearAll_ = nullptr, *searchBtn_ = nullptr;
    RetroActionButton* addBtn_ = nullptr;
    brls::Label* status_ = nullptr;
    SmoothRecyclerFrame* recycler_ = nullptr;
    brls::Box* host_ = nullptr;
    PickerDataSource* data_ = nullptr;
};

// ============================================================================
// Multi-emulator management view
// ============================================================================

struct EmuInstallJob {
    std::atomic<bool> alive{true}, cancelled{false};
    std::atomic<uint64_t> received{0}, total{0};
};

class RetroSetupWizard : public brls::Box {
public:
    RetroSetupWizard(std::string initial, std::shared_ptr<RetroRomStore> store,
                     std::shared_ptr<RetroEmulatorInstaller> installer,
                     std::function<void()> onSaved)
        : brls::Box(brls::Axis::COLUMN),
          store_(std::move(store)),
          installer_(std::move(installer)),
          onSaved_(std::move(onSaved)),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        setGrow(1);
        std::string err;
        if (store_)
            store_->loadSettings(draft_, err);
        if (!initial.empty() && findRetroConsole(initial))
            selectedConsole_ = std::move(initial);
        else if (!draft_.lastConsole.empty() && findRetroConsole(draft_.lastConsole))
            selectedConsole_ = draft_.lastConsole;
        else if (!retroConsoles().empty())
            selectedConsole_ = retroConsoles().front().id;
        else
            selectedConsole_ = "gba";

        auto* header = new brls::Box(brls::Axis::ROW);
        header->setShrink(0);
        header->setPadding(12, 32, 8, 32);
        header->setAlignItems(brls::AlignItems::CENTER);
        title_ = new brls::Label();
        title_->setFontSize(theme::kFontBody);
        title_->setSingleLine(true);
        title_->setGrow(1);
        header->addView(title_);
        stepLabel_ = new brls::Label();
        stepLabel_->setFontSize(theme::kFontCaption);
        stepLabel_->setTextColor(theme::textSecondary());
        stepLabel_->setSingleLine(true);
        stepLabel_->setMarginLeft(12);
        header->addView(stepLabel_);
        addView(header);

        scroll_ = new StickScrollingFrame();
        scroll_->setGrow(1);
        content_ = new brls::Box(brls::Axis::COLUMN);
        content_->setPadding(8, 32, 24, 32);
        scroll_->setContentView(content_);
        addView(scroll_);

        auto* nav = new brls::Box(brls::Axis::ROW);
        nav->setShrink(0);
        nav->setPadding(10, 32, 16, 32);
        nav->setAlignItems(brls::AlignItems::CENTER);
        backBtn_ = makeButton(nav, tr("pipensx/common/cancel"), [this] { onBack(); }, 48.0f);
        backBtn_->setMarginBottom(0);
        backBtn_->setWidth(200);
        auto* spacer = new brls::Box();
        spacer->setGrow(1);
        nav->addView(spacer);
        nextBtn_ = makeButton(nav, tr("pipensx/retro/wizard_next"), [this] { onNext(); }, 48.0f);
        nextBtn_->setMarginBottom(0);
        nextBtn_->setWidth(220);
        addView(nav);

        renderPage(false);
    }

    ~RetroSetupWizard() override { *alive_ = false; }

private:
    struct Recommendation {
        const RetroEmulatorProfile* profile = nullptr;
        std::string detectedPath;
        std::string reason;
    };

    static std::string mapValueOrDefault(const std::map<std::string, std::string>& m,
                                         const std::string& key) {
        const auto it = m.find(key);
        return (it != m.end() && !it->second.empty()) ? it->second : tr("pipensx/retro/wizard_default");
    }

    std::string detectedPathFor(const std::string& emuId) const {
        std::string path, error;
        return installer_->detect(emuId, path, error) ? path : std::string{};
    }

    std::vector<std::string> targetConsoles() const {
        std::vector<std::string> out;
        if (scopeAll_) {
            for (const auto& c : retroConsoles())
                out.push_back(c.id);
        }
        if (out.empty())
            out.push_back(selectedConsole_);
        return out;
    }

    Recommendation recommendFor(const std::string& cid) const {
        const auto list = emulatorsForConsole(cid);
        if (list.empty())
            return {};
        for (const auto* profile : list) {
            const auto path = detectedPathFor(draft_.detectionFor(cid, profile->id));
            if (!path.empty())
                return {profile, path, tr("pipensx/retro/wizard_reason_installed_compat")};
        }
        return {list.front(), {}, tr("pipensx/retro/wizard_reason_app_mode")};
    }

    void onBack() {
        if (step_ == 0) {
            brls::Application::popActivity();
            return;
        }
        --step_;
        renderPage(true);
    }

    void onNext() {
        const size_t lastStep = targetConsoles().size() + 1;
        if (step_ >= lastStep) {
            applyOnce();
            return;
        }
        ++step_;
        renderPage(true);
    }

    void scheduleRender(bool focusContent = false) {
        brls::sync([this, alive = alive_, focusContent] {
            if (*alive)
                renderPage(focusContent);
        });
    }

    void renderPage(bool focusContent = false) {
        const auto targets = targetConsoles();
        const size_t totalSteps = targets.size() + 2;
        if (step_ >= totalSteps)
            step_ = totalSteps - 1;

        const bool hadContentFocus = viewContains(content_, brls::Application::getCurrentFocus());
        if (hadContentFocus)
            brls::Application::giveFocus(backBtn_);
        content_->clearViews();
        firstFocus_ = nullptr;

        nextBtn_->setVisibility(step_ == 0 ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        stepLabel_->setText(std::to_string(step_ + 1) + " / " + std::to_string(totalSteps));
        backBtn_->setText(step_ == 0 ? tr("pipensx/common/cancel") : tr("pipensx/retro/wizard_back"));
        nextBtn_->setText(step_ + 1 == totalSteps ? tr("pipensx/retro/wizard_apply") : tr("pipensx/retro/wizard_next"));
        nextBtn_->setState(applied_ ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED, true);

        if (step_ == 0)
            renderScopePage();
        else if (step_ <= targets.size())
            renderPlatformPage(targets[step_ - 1]);
        else
            renderSummaryPage(targets);

        if (focusContent || hadContentFocus)
            brls::Application::giveFocus(firstFocus_ ? firstFocus_ : static_cast<brls::View*>(nextBtn_));
    }

    void renderScopePage() {
        title_->setText(tr("pipensx/retro/wizard_scope_title"));
        makeLabel(content_, tr("pipensx/retro/wizard_scope_desc"), theme::kFontSmall, theme::textSecondary());
        firstFocus_ = makeButton(content_, consoleFullName(selectedConsole_), [this] {
            scopeAll_ = false;
            onNext();
        });
        makeButton(content_, tr("pipensx/retro/select_console"), [this] { pickSingleConsole(); });
        makeButton(content_, tr("pipensx/retro/all_consoles"), [this] {
            scopeAll_ = true;
            onNext();
        });
    }

    void pickSingleConsole() {
        const auto& consoles = retroConsoles();
        if (consoles.empty())
            return;
        std::vector<std::string> labels;
        int sel = 0;
        for (size_t i = 0; i < consoles.size(); ++i) {
            if (consoles[i].id == selectedConsole_)
                sel = static_cast<int>(i);
            labels.push_back("[" + consoles[i].shortBadge + "] " + consoles[i].defaultName);
        }
        auto* menu = new brls::Dropdown(
            tr("pipensx/retro/select_console"), labels,
            [this, alive = alive_, consoles](int idx) {
                if (!*alive || idx < 0 || static_cast<size_t>(idx) >= consoles.size())
                    return;
                selectedConsole_ = consoles[static_cast<size_t>(idx)].id;
                scopeAll_ = false;
                onNext();
            },
            sel);
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    void renderPlatformPage(const std::string& cid) {
        title_->setText("[" + consoleShortBadge(cid) + "] " + consoleFullName(cid));
        const std::string curEmu = draft_.emulatorFor(cid);
        const auto* prof = findRetroEmulatorProfile(curEmu);
        const bool isCustom = isCustomEmulatorPath(curEmu);
        const auto rec = recommendFor(cid);
        const auto ovIt = draft_.nroPaths.find(cid);
        const std::string nroOverride = (ovIt != draft_.nroPaths.end()) ? ovIt->second : std::string();
        const std::string detected = detectedPathFor(curEmu);
        const std::string effectivePath = isCustom ? curEmu : (!nroOverride.empty() ? nroOverride : detected);

        if (rec.profile) {
            makeLabel(content_,
                      tr("pipensx/retro/wizard_recommended") + ": " + emulatorLabelWithRuntimeTag(*rec.profile) +
                          " - " + rec.reason,
                      theme::kFontSmall, theme::accent());
        }
        makeLabel(content_,
                  tr("pipensx/retro/wizard_detected_path") + ": " +
                      (effectivePath.empty() ? tr("pipensx/retro/wizard_not_detected") : effectivePath),
                  theme::kFontCaption, theme::textSecondary());

        if (rec.profile && curEmu != rec.profile->id) {
            firstFocus_ = makeButton(content_,
                                     tr("pipensx/retro/wizard_use_recommended") + ": " + rec.profile->displayName,
                                     [this, cid, id = rec.profile->id] {
                                         draft_.defaultEmulator[cid] = id;
                                         draft_.nroPaths.erase(cid);
                                         scheduleRender(true);
                                     });
        }

        auto* emuBtn = makeButton(content_,
                                  tr("pipensx/retro/wizard_emulator") + ": " +
                                      (prof ? emulatorLabelWithRuntimeTag(*prof) : emulatorDisplayName(curEmu)),
                                  [this, cid] { pickEmulator(cid); });
        if (!firstFocus_)
            firstFocus_ = emuBtn;

        if (isCustom) {
            makeButton(content_, tr("pipensx/retro/custom_nro") + ": " + curEmu,
                       [this, cid, curEmu] { promptCustomNro(cid, curEmu, false); });
        } else {
            const std::string init = nroOverride.empty() ? detected : nroOverride;
            makeButton(content_,
                       tr("pipensx/retro/wizard_nro_override") + ": " +
                           (nroOverride.empty() ? tr("pipensx/retro/wizard_default") : nroOverride),
                       [this, cid, init] { promptCustomNro(cid, init, true); });
            if (!nroOverride.empty()) {
                makeButton(content_, tr("pipensx/retro/wizard_clear_nro_override"), [this, cid] {
                    draft_.nroPaths.erase(cid);
                    scheduleRender(true);
                });
            }
        }

        makeButton(content_, tr("pipensx/retro/wizard_rom_dir") + ": " + draft_.romDirFor(cid),
                   [this, cid] { promptRomDir(cid); });

        const bool supportsRaDirs = prof && prof->isRetroArchCore;
        const auto bios = configuredBiosStatus(draft_, cid, curEmu);
        const std::string biosHint = bios.present || bios.hintKey.empty() ? std::string{} :
            brls::getStr(bios.hintKey.c_str(), bios.expectedPath);
        if (supportsRaDirs) {
            if (!biosHint.empty())
                makeLabel(content_, tr("pipensx/retro/wizard_bios_hint") + ": " + biosHint, theme::kFontCaption,
                          theme::textSecondary());
            makeButton(content_, tr("pipensx/retro/wizard_bios_dir") + ": " + mapValueOrDefault(draft_.biosDirs, cid),
                       [this, cid] { promptOptionalDir(cid, draft_.biosDirs, tr("pipensx/retro/wizard_bios_dir")); });
            makeButton(content_, tr("pipensx/retro/wizard_save_dir") + ": " + mapValueOrDefault(draft_.saveDirs, cid),
                       [this, cid] { promptOptionalDir(cid, draft_.saveDirs, tr("pipensx/retro/wizard_save_dir")); });
            makeButton(content_, tr("pipensx/retro/wizard_state_dir") + ": " + mapValueOrDefault(draft_.stateDirs, cid),
                       [this, cid] { promptOptionalDir(cid, draft_.stateDirs, tr("pipensx/retro/wizard_state_dir")); });
        } else {
            if (!biosHint.empty())
                makeLabel(content_, tr("pipensx/retro/wizard_bios_hint") + ": " + biosHint, theme::kFontSmall,
                          theme::textSecondary());
            makeLabel(content_, tr("pipensx/retro/wizard_standalone_guidance"), theme::kFontSmall,
                      theme::textSecondary());
        }
    }

    void pickEmulator(const std::string& cid) {
        const auto list = emulatorsForConsole(cid);
        const std::string curEmu = draft_.emulatorFor(cid);
        std::vector<std::string> labels;
        int sel = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i]->id == curEmu)
                sel = static_cast<int>(i);
            std::string lbl = emulatorLabelWithRuntimeTag(*list[i]);
            if (!detectedPathFor(list[i]->id).empty())
                lbl += " [" + tr("pipensx/retro/wizard_installed") + "]";
            labels.push_back(std::move(lbl));
        }
        labels.push_back(tr("pipensx/retro/custom_nro"));
        if (isCustomEmulatorPath(curEmu))
            sel = static_cast<int>(list.size());

        auto* menu = new brls::Dropdown(
            tr("pipensx/retro/wizard_emulator"), labels,
            [this, alive = alive_, cid, list, curEmu](int idx) {
                if (!*alive || idx < 0 || static_cast<size_t>(idx) > list.size())
                    return;
                if (static_cast<size_t>(idx) == list.size()) {
                    promptCustomNro(cid, isCustomEmulatorPath(curEmu) ? curEmu : std::string(), false);
                    return;
                }
                const std::string nextId = list[static_cast<size_t>(idx)]->id;
                if (curEmu != nextId)
                    draft_.nroPaths.erase(cid);
                draft_.defaultEmulator[cid] = nextId;
                scheduleRender(true);
            },
            sel);
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    void promptCustomNro(const std::string& cid, const std::string& initialPath, bool asOverride) {
        promptCustomNroPath(initialPath, [this, alive = alive_, cid, asOverride](const std::string& path) {
            if (!*alive)
                return;
            if (asOverride) {
                draft_.nroPaths[cid] = path;
            } else {
                if (draft_.emulatorFor(cid) != path)
                    draft_.nroPaths.erase(cid);
                draft_.defaultEmulator[cid] = path;
            }
            scheduleRender(true);
        });
    }

    void promptRomDir(const std::string& cid) {
        brls::Application::getImeManager()->openForText(
            [this, alive = alive_, cid](std::string val) {
                if (!*alive)
                    return;
                val = RetroRomStore::normalizeDestination(val);
                if (!RetroRomStore::acceptsDestination(val)) {
                    brls::Application::notify(tr("pipensx/retro/wizard_invalid_dir"));
                    return;
                }
                draft_.romDirs[cid] = val;
                scheduleRender(true);
            },
            tr("pipensx/retro/wizard_rom_dir"), tr("pipensx/retro/wizard_rom_dir_hint"), 256, draft_.romDirFor(cid));
    }

    void promptOptionalDir(const std::string& cid, std::map<std::string, std::string>& target,
                           const std::string& label) {
        const auto it = target.find(cid);
        const std::string cur = (it != target.end()) ? it->second : std::string();
        brls::Application::getImeManager()->openForText(
            [this, alive = alive_, cid, &target](std::string val) {
                if (!*alive)
                    return;
                
                if (val.empty()) {
                    target.erase(cid);
                    scheduleRender(true);
                    return;
                }
                if (!RetroRomStore::acceptsSetupDirectory(val)) {
                    brls::Application::notify(tr("pipensx/retro/wizard_invalid_dir"));
                    return;
                }
                val = RetroRomStore::normalizeDestination(val);
                if (val.empty())
                    target.erase(cid);
                else
                    target[cid] = std::move(val);
                scheduleRender(true);
            },
            label, tr("pipensx/retro/wizard_optional_dir_hint"), 256, cur);
    }

    void renderSummaryPage(const std::vector<std::string>& targets) {
        title_->setText(tr("pipensx/retro/wizard_summary_title"));
        makeLabel(content_, tr("pipensx/retro/wizard_summary_desc"), theme::kFontSmall, theme::textSecondary());
        for (size_t i = 0; i < targets.size(); ++i) {
            const std::string& cid = targets[i];
            const std::string curEmu = draft_.emulatorFor(cid);
            const auto* prof = findRetroEmulatorProfile(curEmu);
            const bool ra = prof && prof->isRetroArchCore;
            const auto ovIt = draft_.nroPaths.find(cid);
            const std::string ov = (ovIt != draft_.nroPaths.end()) ? ovIt->second : std::string();
            const std::string detected = detectedPathFor(curEmu);
            const std::string effPath = isCustomEmulatorPath(curEmu) ? curEmu : (!ov.empty() ? ov : detected);

            auto* card = new brls::Box(brls::Axis::COLUMN);
            card->setFocusable(true);
            card->setPadding(10, 14, 10, 14);
            card->setCornerRadius(theme::kRadiusSmall);
            card->setMarginBottom(10);
            card->registerClickAction([this, targetStep = i + 1](brls::View*) {
                step_ = targetStep;
                scheduleRender(true);
                return true;
            });
            makeLabel(card, "[" + consoleShortBadge(cid) + "] " + consoleFullName(cid), theme::kFontBody,
                      theme::accent());
            std::string details = tr("pipensx/retro/wizard_emulator") + ": " +
                                  (prof ? emulatorLabelWithRuntimeTag(*prof) : emulatorDisplayName(curEmu)) + "\n" +
                                  tr("pipensx/retro/wizard_nro_override") + ": " +
                                  (effPath.empty() ? tr("pipensx/retro/wizard_not_detected") : effPath) + "\n" +
                                  tr("pipensx/retro/wizard_rom_dir") + ": " + draft_.romDirFor(cid);
            if (ra) {
                details += "\n" + tr("pipensx/retro/wizard_bios_dir") + ": " + mapValueOrDefault(draft_.biosDirs, cid) +
                           "\n" + tr("pipensx/retro/wizard_save_dir") + ": " + mapValueOrDefault(draft_.saveDirs, cid) +
                           "\n" + tr("pipensx/retro/wizard_state_dir") + ": " + mapValueOrDefault(draft_.stateDirs, cid);
            } else {
                details += "\n" + tr("pipensx/retro/wizard_standalone_summary");
            }
            makeLabel(card, details, theme::kFontCaption, theme::textSecondary());
            content_->addView(card);
            if (!firstFocus_)
                firstFocus_ = card;
        }
    }

    void applyOnce() {
        if (applied_ || !store_)
            return;
        std::string err;
        if (!store_->saveSettings(draft_, err)) {
            brls::Application::notify(localizeRetroError(err));
            return;
        }
        applied_ = true;
        nextBtn_->setState(brls::ButtonState::DISABLED, true);
        if (onSaved_)
            onSaved_();
        brls::Application::notify(tr("pipensx/retro/default_saved"));
        brls::Application::popActivity();
    }

    std::shared_ptr<RetroRomStore> store_;
    std::shared_ptr<RetroEmulatorInstaller> installer_;
    std::function<void()> onSaved_;
    std::shared_ptr<std::atomic<bool>> alive_;
    RetroSettings draft_;
    std::string selectedConsole_;
    bool scopeAll_ = false;
    bool applied_ = false;
    size_t step_ = 0;
    brls::Label *title_ = nullptr, *stepLabel_ = nullptr;
    StickScrollingFrame* scroll_ = nullptr;
    brls::Box* content_ = nullptr;
    brls::View* firstFocus_ = nullptr;
    RetroActionButton *backBtn_ = nullptr, *nextBtn_ = nullptr;
};

class RetroEmulatorsView : public brls::Box {
public:
    RetroEmulatorsView(std::string consoleFilter,
                       std::vector<RetroEmulatorPackage> catalogPackages,
                       std::shared_ptr<RetroEmulatorInstaller> installer,
                       std::shared_ptr<RetroRomStore> store,
                       std::function<void()> onChanged)
        : brls::Box(brls::Axis::COLUMN),
          consoleFilter_(std::move(consoleFilter)),
          catalogPackages_(std::move(catalogPackages)),
          installer_(std::move(installer)),
          store_(std::move(store)),
          onChanged_(std::move(onChanged)),
          job_(std::make_shared<EmuInstallJob>()) {
        setGrow(1);
        std::string err;
        store_->loadSettings(settings_, err);

        auto* bar = new brls::Box(brls::Axis::ROW);
        bar->setShrink(0);
        bar->setPadding(14, 32, 8, 32);
        bar->setAlignItems(brls::AlignItems::CENTER);

        wizardBtn_ = catalogChip(tr("pipensx/retro/wizard_title"), [this] {
            if (busy_) return;
            auto* wizard = new RetroSetupWizard(consoleFilter_, store_, installer_, [this, job = job_] {
                if (!job->alive) return;
                std::string error;
                store_->loadSettings(settings_, error);
                refreshList();
                if (onChanged_) onChanged_();
            });
            pushDetailActivity(wizard, tr("pipensx/retro/wizard_title"));
        });
        wizardBtn_->setMarginLeft(0);
        bar->addView(wizardBtn_);

        customBtn_ = catalogChip(tr("pipensx/retro/custom_nro"), [this] { setCustomDefault(); });
        bar->addView(customBtn_);

        cancelBtn_ = catalogChip(tr("pipensx/common/cancel"), [this] {
            job_->cancelled = true;
            status_->setText(tr("pipensx/retro/cancelling"));
        });
        cancelBtn_->setVisibility(brls::Visibility::GONE);
        bar->addView(cancelBtn_);

        status_ = new brls::Label();
        status_->setFontSize(theme::kFontSmall);
        status_->setTextColor(theme::textSecondary());
        status_->setSingleLine(true);
        status_->setClipsToBounds(true);
        status_->setGrow(1);
        status_->setShrink(1);
        status_->setMinWidth(0);
        status_->setMarginLeft(16);
        status_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        bar->addView(status_);
        addView(bar);

        scroll_ = new StickScrollingFrame();
        scroll_->setGrow(1);
        list_ = new brls::Box(brls::Axis::COLUMN);
        list_->setPadding(8, 32, 24, 32);
        scroll_->setContentView(list_);
        addView(scroll_);

        refreshList();
    }

    ~RetroEmulatorsView() override {
        job_->alive = false;
        job_->cancelled = true;
    }

    void draw(NVGcontext* vg, float x, float y, float w, float h, brls::Style style,
              brls::FrameContext* ctx) override {
        if (busy_ && !job_->cancelled) {
            const uint64_t rec = job_->received;
            const uint64_t tot = job_->total;
            if (rec != lastRec_) {
                lastRec_ = rec;
                if (tot > 0) {
                    const int pct = static_cast<int>(std::min<uint64_t>(100, rec * 100 / tot));
                    status_->setText(pct < 100
                                         ? tr("pipensx/retro/downloading", pct)
                                         : tr("pipensx/retro/installing"));
                } else if (rec > 0) {
                    status_->setText(tr("pipensx/retro/downloading_bytes", formatBytes(rec)));
                }
            }
        }
        brls::Box::draw(vg, x, y, w, h, style, ctx);
    }

private:
    static void addStatusBadge(brls::Box* parent, const std::string& text,
                               NVGcolor fg, NVGcolor bg) {
        auto* pill = new brls::Box(brls::Axis::ROW);
        pill->setAlignItems(brls::AlignItems::CENTER);
        pill->setPadding(4, 10, 4, 10);
        pill->setCornerRadius(theme::kRadiusSmall);
        pill->setBackgroundColor(bg);
        pill->setMarginLeft(8);
        pill->setShrink(0);

        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(theme::kFontCaption);
        label->setTextColor(fg);
        label->setSingleLine(true);
        pill->addView(label);
        parent->addView(pill);
    }

    void setCustomDefault() {
        if (busy_ || consoleFilter_.empty()) {
            brls::Application::notify(tr("pipensx/retro/pick_console_first"));
            return;
        }
        promptCustomNroPath(settings_.emulatorFor(consoleFilter_), [this, job = job_](const std::string& path) {
            if (!job->alive)
                return;
            if (settings_.emulatorFor(consoleFilter_) != path)
                settings_.nroPaths.erase(consoleFilter_);
            settings_.defaultEmulator[consoleFilter_] = path;
            std::string err;
            store_->saveSettings(settings_, err);
            if (onChanged_)
                onChanged_();
            refreshList();
            brls::Application::notify(tr("pipensx/retro/default_saved"));
        });
    }

    void refreshList() {
        if (viewContains(list_, brls::Application::getCurrentFocus()))
            brls::Application::giveFocus(wizardBtn_);
        list_->clearViews();

        const bool canCustom = !consoleFilter_.empty() && !busy_;
        if (!canCustom && viewContains(customBtn_, brls::Application::getCurrentFocus()))
            brls::Application::giveFocus(wizardBtn_);
        customBtn_->setVisibility(consoleFilter_.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);

        // If filtered to a single console, check BIOS for default/installed emulator and show banner
        if (!consoleFilter_.empty()) {
            const auto activeEmu = settings_.emulatorFor(consoleFilter_);
            const auto bios = configuredBiosStatus(settings_, consoleFilter_, activeEmu);
            if ((bios.required || bios.optional) && !bios.present)
                list_->addView(makeBiosBanner(bios));
        }

        std::vector<const RetroEmulatorProfile*> profiles;
        if (consoleFilter_.empty()) {
            for (const auto& p : retroEmulatorProfiles())
                profiles.push_back(&p);
        } else {
            profiles = emulatorsForConsole(consoleFilter_);
        }

        // Populate rows and check detect state asynchronously
        struct RowState {
            const RetroEmulatorProfile* profile;
            bool installed;
            std::string detectedPath;
        };

        status_->setText(tr("pipensx/retro/emulators_count", profiles.size()));
        const auto job = job_;
        const bool needCatalog = catalogPackages_.empty();
        brls::async([this, job, profiles, installer = installer_, store = store_, needCatalog,
                     filter = consoleFilter_,
                     active = consoleFilter_.empty() ? std::string() : settings_.emulatorFor(consoleFilter_)] {
            std::vector<RetroEmulatorPackage> loadedPkgs;
            if (needCatalog && store)
                loadedPkgs = loadCachedEmulatorPackages(store->appRoot());
            std::vector<RowState> rows;
            rows.reserve(profiles.size());
            for (const auto* p : profiles) {
                std::string path, err;
                const bool det = installer->detect(p->id, path, err);
                rows.push_back({p, det, path});
            }
            brls::sync([this, job, rows = std::move(rows), loadedPkgs = std::move(loadedPkgs),
                        filter, active]() mutable {
                if (!job->alive)
                    return;
                if (catalogPackages_.empty() && !loadedPkgs.empty())
                    catalogPackages_ = std::move(loadedPkgs);
                const bool focusWasInList = viewContains(list_, brls::Application::getCurrentFocus());
                if (focusWasInList)
                    brls::Application::giveFocus(wizardBtn_);
                list_->clearViews();

                if (!filter.empty()) {
                    const auto bios = configuredBiosStatus(settings_, filter, active);
                    if ((bios.required || bios.optional) && !bios.present)
                        list_->addView(makeBiosBanner(bios));
                }

                brls::View* firstCard = nullptr;
                for (const auto& row : rows) {
                    const auto* p = row.profile;
                    const RetroEmulatorPackage pkg =
                        RetroEmulatorInstaller::resolvePackage(p->id, catalogPackages_);

                    auto* card = new brls::Box(brls::Axis::ROW);
                    card->setFocusable(true);
                    card->setAlignItems(brls::AlignItems::CENTER);
                    card->setPadding(16, 20, 16, 20);
                    card->setMinHeight(86);
                    card->setShrink(0);
                    card->setMarginBottom(12);
                    card->setCornerRadius(theme::kRadiusMedium);
                    card->setHighlightCornerRadius(theme::kRadiusMedium);
                    card->setBackgroundColor(theme::surface());
                    card->addGestureRecognizer(new brls::TapGestureRecognizer(card));

                    auto* info = new brls::Box(brls::Axis::COLUMN);
                    info->setGrow(1);
                    info->setShrink(1);
                    info->setMinWidth(0);

                    auto* title = new brls::Label();
                    title->setText(p->displayName);
                    title->setFontSize(theme::kFontBody);
                    title->setTextColor(theme::textPrimary());
                    title->setSingleLine(true);
                    info->addView(title);

                    const std::string verText =
                        pkg.version == "latest" ? std::string("nightly") : ("v" + pkg.version);
                    const std::string runtimeText =
                        p->isRetroArchCore ? std::string("RetroArch")
                                           : tr("pipensx/retro/runtime_standalone");
                    std::string metaText = p->author + " · " + verText + " · " + runtimeText;
                    if (filter.empty()) {
                        std::string consolesStr;
                        for (size_t i = 0; i < p->supportedConsoles.size(); ++i) {
                            if (i > 0) consolesStr += ", ";
                            consolesStr += consoleShortBadge(p->supportedConsoles[i]);
                        }
                        if (!consolesStr.empty())
                            metaText += " · " + consolesStr;
                    }

                    auto* meta = new brls::Label();
                    meta->setText(metaText);
                    meta->setFontSize(theme::kFontCaption);
                    meta->setTextColor(theme::textSecondary());
                    meta->setSingleLine(true);
                    meta->setMarginTop(4);
                    info->addView(meta);

                    auto* pathLabel = new brls::Label();
                    pathLabel->setText(p->nroRelativePath);
                    pathLabel->setFontSize(theme::kFontCaption);
                    pathLabel->setTextColor(theme::textTertiary());
                    pathLabel->setSingleLine(true);
                    pathLabel->setMarginTop(2);
                    info->addView(pathLabel);

                    card->addView(info);

                    auto* right = new brls::Box(brls::Axis::ROW);
                    right->setAlignItems(brls::AlignItems::CENTER);
                    right->setShrink(0);
                    right->setMarginLeft(16);

                    const bool isDefault = !filter.empty() && active == p->id;
                    if (isDefault)
                        addStatusBadge(right, tr("pipensx/retro/default_emulator"),
                                       theme::accent(), theme::sidebarActive());
                    if (row.installed)
                        addStatusBadge(right, tr("pipensx/retro/installed_badge"),
                                       theme::success(), theme::panel());
                    else if (RetroEmulatorInstaller::supports(pkg))
                        addStatusBadge(right,
                                       pkg.bytes > 0 ? formatBytes(pkg.bytes) : std::string("nightly"),
                                       theme::textSecondary(), theme::panel());

                    auto* chevron = new brls::Label();
                    chevron->setText("›");
                    chevron->setFontSize(theme::kFontHeading);
                    chevron->setTextColor(theme::textTertiary());
                    chevron->setMarginLeft(12);
                    chevron->setShrink(0);
                    right->addView(chevron);

                    card->addView(right);
                    card->registerClickAction([this, p, pkg, installed = row.installed](brls::View*) {
                        if (!busy_)
                            openProfileMenu(p, pkg, installed);
                        return true;
                    });
                    card->updateActionHint(brls::BUTTON_A, tr("pipensx/installed/actions"));
                    list_->addView(card);
                    if (!firstCard)
                        firstCard = card;
                }

                if (firstCard && !busy_ && (!initialFocused_ || focusWasInList)) {
                    initialFocused_ = true;
                    brls::Application::giveFocus(firstCard);
                }
            });
        });
    }

    void openProfileMenu(const RetroEmulatorProfile* p, const RetroEmulatorPackage& pkg, bool installed) {
        std::vector<std::string> options;
        std::vector<std::function<void()>> actions;

        if (!installed) {
            options.push_back(tr("pipensx/retro/install_emulator", p->displayName));
            actions.push_back([this, p, pkg] { startInstall(p, pkg, false, false); });
            if (!consoleFilter_.empty() && settings_.emulatorFor(consoleFilter_) != p->id) {
                options.push_back(tr("pipensx/retro/install_and_default", p->displayName));
                actions.push_back([this, p, pkg] { startInstall(p, pkg, false, true); });
            }
        } else {
            if (!consoleFilter_.empty() && settings_.emulatorFor(consoleFilter_) != p->id) {
                options.push_back(tr("pipensx/retro/set_default"));
                actions.push_back([this, p] {
                    if (settings_.emulatorFor(consoleFilter_) != p->id)
                        settings_.nroPaths.erase(consoleFilter_);
                    settings_.defaultEmulator[consoleFilter_] = p->id;
                    std::string err;
                    store_->saveSettings(settings_, err);
                    if (onChanged_)
                        onChanged_();
                    refreshList();
                    brls::Application::notify(tr("pipensx/retro/default_saved"));
                });
            }
            options.push_back(tr("pipensx/retro/reinstall_emulator"));
            actions.push_back([this, p, pkg] { startInstall(p, pkg, true, false); });
            options.push_back(tr("pipensx/retro/uninstall_emulator"));
            actions.push_back([this, p] { confirmUninstall(p); });
        }

        const auto job = job_;
        auto* menu = new brls::Dropdown(
            p->displayName, options,
            [job, actions](int index) {
                if (!job->alive || index < 0 || static_cast<size_t>(index) >= actions.size())
                    return;
                brls::sync(actions[static_cast<size_t>(index)]);
            });
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    void startInstall(const RetroEmulatorProfile* p, const RetroEmulatorPackage& pkg,
                      bool replace, bool alsoSetDefault) {
        if (busy_)
            return;
        if (!RetroEmulatorInstaller::supports(pkg)) {
            brls::Application::notify(tr("pipensx/retro/unsupported_package", pkg.version));
            return;
        }
        busy_ = true;
        lastRec_ = UINT64_MAX;
        job_->cancelled = false;
        job_->received = 0;
        job_->total = pkg.bytes;
        cancelBtn_->setVisibility(brls::Visibility::VISIBLE);
        brls::Application::giveFocus(cancelBtn_);
        status_->setText(tr("pipensx/retro/downloading", 0));

        const auto job = job_;
        brls::async([this, job, p, pkg, replace, alsoSetDefault, installer = installer_] {
            std::string error;
            const bool ok = installer->install(
                pkg, job->cancelled,
                [job](uint64_t bytes, uint64_t totalBytes) {
                    job->received = bytes;
                    if (totalBytes > 0)
                        job->total = totalBytes;
                },
                error, replace);
            brls::sync([this, job, p, ok, error, alsoSetDefault] {
                if (!job->alive)
                    return;
                busy_ = false;
                if (viewContains(cancelBtn_, brls::Application::getCurrentFocus()))
                    brls::Application::giveFocus(wizardBtn_);
                cancelBtn_->setVisibility(brls::Visibility::GONE);
                if (ok) {
                    if (alsoSetDefault && !consoleFilter_.empty()) {
                        if (settings_.emulatorFor(consoleFilter_) != p->id)
                            settings_.nroPaths.erase(consoleFilter_);
                        settings_.defaultEmulator[consoleFilter_] = p->id;
                        std::string err;
                        store_->saveSettings(settings_, err);
                    }
                    if (onChanged_)
                        onChanged_();
                    status_->setText(tr("pipensx/retro/emulator_ready", p->displayName));
                    brls::Application::notify(tr("pipensx/retro/emulator_ready", p->displayName));
                    refreshList();
                } else {
                    status_->setText(job->cancelled
                                         ? tr("pipensx/retro/cancelled")
                                         : tr("pipensx/retro/install_failed", localizeRetroError(error)));
                }
            });
        });
    }

    void confirmUninstall(const RetroEmulatorProfile* p) {
        const auto job = job_;
        auto* dialog = new brls::Dialog(tr("pipensx/retro/uninstall_confirm", p->displayName));
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->addButton(tr("pipensx/common/remove"), [this, job, p] {
            if (!job->alive)
                return;
            brls::async([this, job, p, installer = installer_] {
                std::string error;
                const bool ok = installer->uninstall(p->id, error);
                brls::sync([this, job, p, ok, error] {
                    if (!job->alive)
                        return;
                    if (ok) {
                        if (onChanged_)
                            onChanged_();
                        brls::Application::notify(tr("pipensx/retro/uninstalled", p->displayName));
                        refreshList();
                    } else {
                        status_->setText(localizeRetroError(error));
                    }
                });
            });
        });
        dialog->open();
    }

    std::string consoleFilter_;
    std::vector<RetroEmulatorPackage> catalogPackages_;
    std::shared_ptr<RetroEmulatorInstaller> installer_;
    std::shared_ptr<RetroRomStore> store_;
    std::function<void()> onChanged_;
    std::shared_ptr<EmuInstallJob> job_;
    RetroSettings settings_;
    theme::Button *wizardBtn_ = nullptr, *customBtn_ = nullptr, *cancelBtn_ = nullptr;
    brls::Label* status_ = nullptr;
    StickScrollingFrame* scroll_ = nullptr;
    brls::Box* list_ = nullptr;
    bool busy_ = false;
    bool initialFocused_ = false;
    uint64_t lastRec_ = UINT64_MAX;
};

// ============================================================================
// Single operation progress & prepare view
// ============================================================================

struct Job {
    std::atomic<bool> alive{true}, cancelled{false};
};

class OperationView : public brls::Box {
public:
    OperationView(RetroRomOperation operation, DownloadManager* manager,
                  std::shared_ptr<RetroRomStore> store,
                  std::shared_ptr<RetroEmulatorInstaller> installer,
                  GameMetadataService* metadata)
        : brls::Box(brls::Axis::COLUMN),
          operation_(std::move(operation)),
          manager_(manager),
          store_(std::move(store)),
          installer_(std::move(installer)),
          metadata_(metadata),
          job_(std::make_shared<Job>()) {
        setGrow(1);
        setPadding(24, 40, 24, 40);

        auto* content = new brls::Box(brls::Axis::COLUMN);
        makeLabel(content, consoleShortBadge(operation_.platform) + " · " +
                               "sdmc:/" + operation_.destination + "/" + operation_.id,
                  theme::kFontSmall, theme::textSecondary());
        makeLabel(content, tr("pipensx/retro/archives_kept"), theme::kFontSmall, theme::textSecondary());
        status_ = makeLabel(content, "");

        prepare_ = makeButton(content, tr("pipensx/retro/prepare"), [this] { prepare(); });
        cancelBtn_ = makeButton(content, tr("pipensx/common/cancel"), [this] {
            job_->cancelled = true;
            status_->setText(tr("pipensx/retro/cancelling"));
        });
        cancelBtn_->setVisibility(brls::Visibility::GONE);
        games_ = makeButton(content, tr("pipensx/retro/choose_games"), [this] { choose(); });
        library_ = makeButton(content, tr("pipensx/retro/add_library"), [this] { addLibrary(); });
        deleteBtn_ = makeButton(content, tr("pipensx/retro/delete_operation"), [this] { confirmDelete(); });

        auto* scroll = new StickScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);
        refresh();
    }

    ~OperationView() override {
        job_->alive = false;
        job_->cancelled = true;
    }

    void draw(NVGcontext* vg, float x, float y, float w, float h, brls::Style style,
              brls::FrameContext* context) override {
        const auto now = std::chrono::steady_clock::now();
        if (!busy_ && !failed_ && now - lastPoll_ > std::chrono::seconds(1)) {
            lastPoll_ = now;
            refresh();
        }
        brls::Box::draw(vg, x, y, w, h, style, context);
    }

private:
    void addLibrary() {
        if (busy_ || !operation_.ready ||
            std::none_of(operation_.games.begin(), operation_.games.end(),
                         [](const auto& game) { return game.selected; }))
            return;
        busy_ = true;
        failed_ = false;
        brls::Application::giveFocus(deleteBtn_);
        for (auto* control : {prepare_, games_, library_})
            control->setState(brls::ButtonState::DISABLED);
        deleteBtn_->setState(brls::ButtonState::DISABLED, true);
        const auto job = job_;
        status_->setText(tr("pipensx/retro/saving_selection"));
        brls::async([this, job, store = store_, id = operation_.id] {
            std::string error;
            const bool ok = store->addSelectedToLibrary(id, error);
            brls::sync([this, job, ok, error] {
                if (!job->alive)
                    return;
                busy_ = false;
                refresh();
                if (viewContains(deleteBtn_, brls::Application::getCurrentFocus())) {
                    brls::Application::giveFocus(library_->enabled()
                                                     ? static_cast<brls::View*>(library_)
                                                     : static_cast<brls::View*>(prepare_));
                }
                if (ok) {
                    brls::Application::notify(tr("pipensx/retro/library_added"));
                    status_->setText(tr("pipensx/retro/library_added"));
                    failed_ = false;
                } else {
                    status_->setText(localizeRetroError(error));
                    failed_ = true;
                }
            });
        });
    }

    void refresh() {
        deleteBtn_->setState(busy_ ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
        library_->setState(!busy_ && operation_.ready &&
                                   std::any_of(operation_.games.begin(), operation_.games.end(),
                                               [](const auto& g) { return g.selected; })
                               ? brls::ButtonState::ENABLED
                               : brls::ButtonState::DISABLED);
        games_->setState(!busy_ && operation_.ready ? brls::ButtonState::ENABLED
                                                    : brls::ButtonState::DISABLED);
        if (operation_.ready) {
            prepare_->setState(busy_ ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED);
            const auto count = std::count_if(operation_.games.begin(), operation_.games.end(),
                                             [](const auto& g) { return g.selected; });
            status_->setText(tr("pipensx/retro/prepared", operation_.games.size(), count));
            return;
        }
        const auto task = manager_ ? manager_->snapshotUi(operation_.taskId) : std::nullopt;
        const bool complete = task && task->mode == TransferMode::DownloadOnly &&
                              task->status == DownloadStatus::Completed;
        prepare_->setState(!busy_ && (complete || !task) ? brls::ButtonState::ENABLED
                                                         : brls::ButtonState::DISABLED);
        status_->setText(tr(!task      ? "pipensx/retro/task_missing"
                            : complete ? "pipensx/retro/download_ready"
                                       : "pipensx/retro/wait_download"));
    }

    void prepare() {
        if (busy_)
            return;
        busy_ = true;
        failed_ = false;
        for (auto* b : {prepare_, games_, library_, deleteBtn_})
            b->setState(brls::ButtonState::DISABLED);
        cancelBtn_->setVisibility(brls::Visibility::VISIBLE);
        cancelBtn_->setState(brls::ButtonState::ENABLED);
        brls::Application::giveFocus(cancelBtn_);
        status_->setText(tr("pipensx/retro/preparing"));
        const auto job = job_;
        job->cancelled = false;
        brls::async([this, job, op = operation_, manager = manager_, store = store_] {
            RetroRomOperation result;
            std::string error;
            bool ok = false;
            if (op.ready || !manager || !manager->hasTask(op.taskId)) {
                ok = store->prepare(op.id, {}, job->cancelled, result, error);
            } else {
                auto lease = manager->beginExternalDeploy(op.taskId, error);
                if (lease && lease->task().mode == TransferMode::DownloadOnly) {
                    TaskFileInventory inventory;
                    if (buildTaskFileInventory(manager->rootPath(), lease->task(), inventory, error) &&
                        inventory.completeManifest) {
                        std::vector<RetroRomInput> files;
                        bool complete = true;
                        for (const auto& file : inventory.files) {
                            if (file.action != TaskFileAction::Skip) {
                                if (file.state != TaskFileState::Present) {
                                    complete = false;
                                    error = "Selected download file is missing or unsafe.";
                                    break;
                                }
                                files.push_back({file.logicalPath, file.absolutePath, file.size});
                            }
                        }
                        if (complete) {
                            auto lastTick = std::make_shared<std::chrono::steady_clock::time_point>();
                            auto prepProgress = [this, job, lastTick](uint64_t doneBytes, uint64_t totalBytes,
                                                                      size_t doneFiles, size_t totalFiles) {
                                const auto now = std::chrono::steady_clock::now();
                                if (lastTick->time_since_epoch().count() != 0 &&
                                    now - *lastTick < std::chrono::milliseconds(200) &&
                                    (totalBytes == 0 || doneBytes < totalBytes))
                                    return;
                                *lastTick = now;
                                std::string line = tr("pipensx/retro/preparing");
                                if (totalBytes > 0) {
                                    const int pct = static_cast<int>(
                                        std::min<uint64_t>(100, (doneBytes * 100) / totalBytes));
                                    line += " " + std::to_string(pct) + "% · " +
                                            formatBytes(doneBytes) + " / " + formatBytes(totalBytes) +
                                            " (" + std::to_string(doneFiles) + " / " +
                                            std::to_string(totalFiles) + ")";
                                }
                                brls::sync([this, job, line = std::move(line)] {
                                    if (job->alive && busy_)
                                        status_->setText(line);
                                });
                            };
                            ok = store->prepare(op.id, files, job->cancelled, result, error, prepProgress);
                            if (ok) {
                                lease.reset();
                                std::string rmErr;
                                manager->remove(op.taskId, true, rmErr);
                            }
                        }
                    }
                }
            }
            brls::sync([this, job, ok, result = std::move(result), error = std::move(error)]() mutable {
                if (!job->alive)
                    return;
                busy_ = false;
                if (viewContains(cancelBtn_, brls::Application::getCurrentFocus()))
                    brls::Application::giveFocus(prepare_);
                cancelBtn_->setVisibility(brls::Visibility::GONE);
                if (ok) {
                    operation_ = std::move(result);
                    refresh();
                } else {
                    refresh();
                    status_->setText(tr("pipensx/retro/preparation_failed", localizeRetroError(error)));
                    failed_ = true;
                }
            });
        });
    }

    void choose() {
        if (busy_ || !operation_.ready)
            return;
        const auto job = job_;
        auto* picker = new RetroGamePickerView(operation_, store_, [this, job](RetroRomOperation updated) {
            if (!job->alive)
                return;
            operation_ = std::move(updated);
            refresh();
        });
        pushDetailActivity(picker, operation_.title);
    }

    void confirmDelete() {
        if (busy_)
            return;
        const std::vector<std::string> options = {
            tr("pipensx/retro/remove_record_only"),
            tr("pipensx/retro/remove_record_and_files"),
        };
        const auto job = job_;
        auto* menu = new brls::Dropdown(
            tr("pipensx/retro/delete_operation_confirm", operation_.title), options,
            [this, job](int index) {
                if (!job->alive || index < 0 || index > 1)
                    return;
                const bool deleteFiles = (index == 1);
                brls::sync([this, job, deleteFiles] {
                    if (!job->alive)
                        return;
                    std::string err;
                    store_->removeOperation(operation_.id, deleteFiles, err);
                    brls::Application::popActivity();
                });
            });
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    RetroRomOperation operation_;
    DownloadManager* manager_;
    std::shared_ptr<RetroRomStore> store_;
    std::shared_ptr<RetroEmulatorInstaller> installer_;
    GameMetadataService* metadata_;
    std::shared_ptr<Job> job_;
    brls::Label* status_ = nullptr;
    RetroActionButton *prepare_ = nullptr, *cancelBtn_ = nullptr, *games_ = nullptr,
                      *library_ = nullptr, *deleteBtn_ = nullptr;
    bool busy_ = false, failed_ = false;
    std::chrono::steady_clock::time_point lastPoll_{};
};

// ============================================================================
// Retro Install Options (Emulator, HOME forwarder, ROM folder, extra settings)
// ============================================================================

std::mutex gAutoPrepMutex;
std::set<std::string> gAutoPrepActive;

bool isRetroTaskAutoPreparing(const std::string& taskId) {
    if (taskId.empty())
        return false;
    std::lock_guard<std::mutex> lock(gAutoPrepMutex);
    return gAutoPrepActive.count(taskId) != 0;
}

class RetroInstallOptionsView : public brls::Box {
public:
    RetroInstallOptionsView(std::string platform,
                            std::string activeEmulatorId,
                            bool canPickFiles,
                            std::shared_ptr<RetroRomStore> store,
                            std::shared_ptr<RetroEmulatorInstaller> installer,
                            std::function<void(const std::string& emulatorId)> onChanged,
                            std::function<void()> onPickFiles)
        : brls::Box(brls::Axis::COLUMN),
          platform_(std::move(platform)),
          activeEmulatorId_(std::move(activeEmulatorId)),
          canPickFiles_(canPickFiles),
          store_(std::move(store)),
          installer_(std::move(installer)),
          onChanged_(std::move(onChanged)),
          onPickFiles_(std::move(onPickFiles)),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        setGrow(1);
        setPadding(16, 36, 16, 36);

        std::string err;
        if (store_)
            store_->loadSettings(settings_, err);

        scroll_ = new StickScrollingFrame();
        scroll_->setGrow(1);
        content_ = new brls::Box(brls::Axis::COLUMN);
        content_->setPadding(8, 8, 24, 8);
        scroll_->setContentView(content_);
        addView(scroll_);

        rebuild(false);
    }

    ~RetroInstallOptionsView() override { *alive_ = false; }

private:
    static std::string mapValueOrDefault(const std::map<std::string, std::string>& m,
                                         const std::string& key) {
        const auto it = m.find(key);
        return (it != m.end() && !it->second.empty()) ? it->second : tr("pipensx/retro/wizard_default");
    }

    std::string detectedPathFor(const std::string& emuId) const {
        if (!installer_ || emuId.empty())
            return {};
        std::string path, err;
        return installer_->detect(emuId, path, err) ? path : std::string{};
    }

    void saveAndRefresh() {
        if (store_) {
            std::string err;
            store_->saveSettings(settings_, err);
        }
        if (onChanged_)
            onChanged_(activeEmulatorId_);
        brls::sync([this, alive = alive_] {
            if (*alive)
                rebuild(true);
        });
    }

    void rebuild(bool restoreFocus) {
        const bool hadFocus = viewContains(content_, brls::Application::getCurrentFocus());
        content_->clearViews();
        brls::View* firstFocus = nullptr;

        const std::string emuId = activeEmulatorId_;
        const auto* prof = findRetroEmulatorProfile(emuId);
        const bool isCustom = isCustomEmulatorPath(emuId);
        const auto ovIt = settings_.nroPaths.find(platform_);
        const std::string nroOverride = (ovIt != settings_.nroPaths.end()) ? ovIt->second : std::string();
        const std::string detected = detectedPathFor(settings_.detectionFor(platform_, emuId));
        const bool emuInstalled = !detected.empty();

        // BIOS / firmware notice at top when missing
        const auto bios = configuredBiosStatus(settings_, platform_, emuId);
        if ((bios.required || bios.optional) && !bios.present) {
            content_->addView(makeBiosBanner(bios));
        }
        if (const auto* con = findRetroConsole(platform_); con && con->packageInstallInEmulator) {
            makeLabel(content_, tr("pipensx/retro/vita_vpk_note"), theme::kFontSmall,
                      theme::color(theme::Tok::ChipWarning));
        }

        // 1. Emulator selection
        std::string emuLabel = tr("pipensx/retro/wizard_emulator") + ": ";
        if (emuId.empty()) {
            emuLabel += tr("pipensx/retro/without_emulator");
        } else {
            emuLabel += (prof ? emulatorLabelWithRuntimeTag(*prof) : emulatorDisplayName(emuId));
            emuLabel += " · " + tr(emuInstalled ? "pipensx/retro/installed_badge"
                                                : "pipensx/retro/auto_install_badge");
        }
        auto* emuBtn = makeButton(content_, emuLabel, [this] { pickEmulator(); });
        firstFocus = emuBtn;

        // 2. Auto-create HOME forwarder toggle
        const bool fwdAllowed = !emuId.empty();
        const bool fwdOn = fwdAllowed && settings_.autoCreateForwarder;
        std::string fwdLabel = tr("pipensx/retro/auto_forwarder") + ": " +
                               tr(fwdOn ? "pipensx/retro/option_on" : "pipensx/retro/option_off");
        auto* fwdBtn = makeButton(content_, fwdLabel, [this] {
            settings_.autoCreateForwarder = !settings_.autoCreateForwarder;
            saveAndRefresh();
        });
        if (!fwdAllowed) {
            fwdBtn->setState(brls::ButtonState::DISABLED);
            makeLabel(content_, tr("pipensx/retro/auto_forwarder_needs_emu"),
                      theme::kFontCaption, theme::textSecondary());
        } else {
            makeLabel(content_, tr("pipensx/retro/auto_forwarder_hint"),
                      theme::kFontCaption, theme::textSecondary());
        }

        // 3. ROM destination folder
        makeButton(content_,
                   tr("pipensx/retro/wizard_rom_dir") + ": sdmc:/" + settings_.romDirFor(platform_),
                   [this] { editRomDir(); });

        // 4. Extra emulator-specific settings when available
        if (!emuId.empty()) {
            if (isCustom) {
                makeButton(content_, tr("pipensx/retro/custom_nro") + ": " + emuId,
                           [this, emuId] { promptNroPath(emuId, false); });
            } else {
                const std::string init = nroOverride.empty() ? detected : nroOverride;
                makeButton(content_,
                           tr("pipensx/retro/wizard_nro_override") + ": " +
                               (nroOverride.empty() ? tr("pipensx/retro/wizard_default") : nroOverride),
                           [this, init] { promptNroPath(init, true); });
                if (!nroOverride.empty()) {
                    makeButton(content_, tr("pipensx/retro/wizard_clear_nro_override"), [this] {
                        settings_.nroPaths.erase(platform_);
                        saveAndRefresh();
                    });
                }
            }

            const bool supportsRaDirs = prof && prof->isRetroArchCore;
            if (supportsRaDirs) {
                makeButton(content_,
                           tr("pipensx/retro/wizard_bios_dir") + ": " +
                               mapValueOrDefault(settings_.biosDirs, platform_),
                           [this] { editOptionalDir(settings_.biosDirs, tr("pipensx/retro/wizard_bios_dir")); });
                makeButton(content_,
                           tr("pipensx/retro/wizard_save_dir") + ": " +
                               mapValueOrDefault(settings_.saveDirs, platform_),
                           [this] { editOptionalDir(settings_.saveDirs, tr("pipensx/retro/wizard_save_dir")); });
                makeButton(content_,
                           tr("pipensx/retro/wizard_state_dir") + ": " +
                               mapValueOrDefault(settings_.stateDirs, platform_),
                           [this] { editOptionalDir(settings_.stateDirs, tr("pipensx/retro/wizard_state_dir")); });
            } else {
                makeLabel(content_, tr("pipensx/retro/wizard_standalone_guidance"),
                          theme::kFontCaption, theme::textSecondary());
            }
        }

        // 5. Optional manual torrent file selection
        if (canPickFiles_ && onPickFiles_) {
            auto* pickBtn = makeButton(content_, tr("pipensx/retro/install_pick_files"), [this] {
                auto cb = onPickFiles_;
                brls::Application::popActivity(brls::TransitionAnimation::FADE, [cb] {
                    if (cb)
                        cb();
                });
            });
            pickBtn->setMarginTop(8);
        }

        if ((restoreFocus || hadFocus) && firstFocus)
            brls::Application::giveFocus(firstFocus);
    }

    void pickEmulator() {
        const auto profiles = emulatorsForConsole(platform_);
        const std::string active = activeEmulatorId_;
        std::vector<std::string> labels;
        std::vector<std::string> ids;
        labels.reserve(profiles.size() + 2);
        ids.reserve(profiles.size() + 2);
        int current = 0;

        for (size_t i = 0; i < profiles.size(); ++i) {
            const bool installed = !detectedPathFor(profiles[i]->id).empty();
            std::string label = emulatorLabelWithRuntimeTag(*profiles[i]);
            label += " · " + tr(installed ? "pipensx/retro/installed_badge"
                                          : "pipensx/retro/auto_install_badge");
            if (profiles[i]->id == active)
                current = static_cast<int>(i);
            labels.push_back(std::move(label));
            ids.push_back(profiles[i]->id);
        }
        labels.push_back(tr("pipensx/retro/custom_nro") + "...");
        ids.push_back("__custom__");
        if (isCustomEmulatorPath(active))
            current = static_cast<int>(profiles.size());
        labels.push_back(tr("pipensx/retro/without_emulator"));
        ids.push_back("");
        if (active.empty())
            current = static_cast<int>(profiles.size() + 1);

        auto* menu = new brls::Dropdown(
            tr("pipensx/retro/select_emulator"), labels,
            [this, alive = alive_, ids = std::move(ids)](int index) {
                if (!*alive || index < 0 || static_cast<size_t>(index) >= ids.size())
                    return;
                const std::string chosen = ids[static_cast<size_t>(index)];
                if (chosen == "__custom__") {
                    promptNroPath(activeEmulatorId_, false);
                    return;
                }
                if (activeEmulatorId_ != chosen)
                    settings_.nroPaths.erase(platform_);
                activeEmulatorId_ = chosen;
                if (!chosen.empty())
                    settings_.defaultEmulator[platform_] = chosen;
                saveAndRefresh();
            },
            current);
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    void promptNroPath(const std::string& initial, bool asOverride) {
        promptCustomNroPath(initial, [this, alive = alive_, asOverride](const std::string& path) {
            if (!*alive)
                return;
            if (asOverride) {
                settings_.nroPaths[platform_] = path;
            } else {
                activeEmulatorId_ = path;
                settings_.defaultEmulator[platform_] = path;
                settings_.nroPaths.erase(platform_);
            }
            saveAndRefresh();
        });
    }

    void editRomDir() {
        const std::string current = settings_.romDirFor(platform_);
        brls::Application::getImeManager()->openForText(
            [this, alive = alive_](std::string value) {
                if (!*alive)
                    return;
                const std::string normalized = RetroRomStore::normalizeDestination(value);
                if (!RetroRomStore::acceptsDestination(normalized)) {
                    brls::Application::notify(tr("pipensx/retro/invalid_folder"));
                    return;
                }
                settings_.romDirs[platform_] = normalized;
                saveAndRefresh();
            },
            tr("pipensx/retro/wizard_rom_dir"), tr("pipensx/retro/wizard_rom_dir_hint"),
            240, current, brls::KEYBOARD_DISABLE_NONE);
    }

    void editOptionalDir(std::map<std::string, std::string>& target, const std::string& title) {
        const auto it = target.find(platform_);
        const std::string current = (it != target.end()) ? it->second : std::string();
        brls::Application::getImeManager()->openForText(
            [this, alive = alive_, &target](std::string value) {
                if (!*alive)
                    return;
                while (!value.empty() && value.back() == ' ')
                    value.pop_back();
                if (value.empty()) {
                    target.erase(platform_);
                    saveAndRefresh();
                    return;
                }
                if (!RetroRomStore::acceptsSetupDirectory(value)) {
                    brls::Application::notify(tr("pipensx/retro/wizard_invalid_dir"));
                    return;
                }
                target[platform_] = RetroRomStore::normalizeDestination(value);
                saveAndRefresh();
            },
            title, tr("pipensx/retro/wizard_optional_dir_hint"),
            240, current, brls::KEYBOARD_DISABLE_NONE);
    }

    std::string platform_;
    std::string activeEmulatorId_;
    bool canPickFiles_ = false;
    std::shared_ptr<RetroRomStore> store_;
    std::shared_ptr<RetroEmulatorInstaller> installer_;
    std::function<void(const std::string&)> onChanged_;
    std::function<void()> onPickFiles_;
    std::shared_ptr<std::atomic<bool>> alive_;
    RetroSettings settings_;
    StickScrollingFrame* scroll_ = nullptr;
    brls::Box* content_ = nullptr;
};

// ============================================================================
// Unified two-column eShop Retro Detail Activity
// ============================================================================

class RetroDetailView : public brls::Box {
public:
    RetroDetailView(RetroRelease release,
                    std::optional<RetroStats> stats,
                    std::vector<RetroEmulatorPackage> catalogEmulators,
                    DownloadManager* manager,
                    AppSettings* settings,
                    std::shared_ptr<RetroRomStore> store,
                    std::shared_ptr<RetroEmulatorInstaller> installer,
                    GameMetadataService* metadata)
        : brls::Box(brls::Axis::ROW),
          release_(std::move(release)),
          stats_(stats),
          catalogEmulators_(std::move(catalogEmulators)),
          manager_(manager),
          settings_(settings),
          store_(std::move(store)),
          installer_(std::move(installer)),
          metadata_(metadata),
          job_(std::make_shared<Job>()) {
        setGrow(1);
        setPadding(20, 36, 20, 36);

        RetroSettings rSettings;
        std::string ignored;
        store_->loadSettings(rSettings, ignored);
        folder_ = rSettings.romDirFor(release_.platform);
        emulatorId_ = rSettings.emulatorFor(release_.platform);
        createForwarder_ = rSettings.autoCreateForwarder;

        // Left sidebar column: cover, Install, Install options, compact summary, StorageMeter
        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidth(340);
        left->setShrink(0);
        left->setMarginRight(24);
        left->setClipsToBounds(true);

        auto* coverBox = new brls::Box(brls::Axis::COLUMN);
        coverBox->setHeight(164);
        coverBox->setShrink(0);
        coverBox->setCornerRadius(theme::kRadiusMedium);
        coverBox->setBackgroundColor(theme::panel());
        coverBox->setAlignItems(brls::AlignItems::CENTER);
        coverBox->setJustifyContent(brls::JustifyContent::CENTER);
        coverBox->setPadding(6, 6, 6, 6);
        auto* cover = new AsyncRgbaImage();
        cover->setWidth(324);
        cover->setHeight(152);
        cover->setScalingType(brls::ImageScalingType::FIT);
        cover->setCornerRadius(theme::kRadiusSmall);
        loadImageInto(cover, metadata_, release_.coverUrl);
        coverBox->addView(cover);
        left->addView(coverBox);

        primaryBtn_ = makeButton(left, tr("pipensx/common/install"), [this] { onPrimary(); }, 52.0f);
        primaryBtn_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        primaryBtn_->setFontSize(theme::kFontBody);
        primaryBtn_->setMarginTop(12);

        cancelBtn_ = makeButton(left, tr("pipensx/common/cancel"), [this] {
            job_->cancelled = true;
            status_->setText(tr("pipensx/retro/cancelling"));
        }, 42.0f);
        cancelBtn_->setVisibility(brls::Visibility::GONE);

        optionsBtn_ = makeButton(left, tr("pipensx/retro/install_options"),
                                 [this] { openInstallOptions(); }, 46.0f);
        optionsBtn_->setFontSize(theme::kFontSmall);

        summaryLabel_ = makeLabel(left, "", theme::kFontCaption, theme::textSecondary());
        summaryLabel_->setMarginTop(2);
        summaryLabel_->setMarginBottom(6);

        meter_ = new StorageMeter(true);
        meter_->setShrink(0);
        meter_->setMarginTop(4);
        meter_->setHeader(tr("pipensx/retro/sd_space"));
        left->addView(meter_);

        status_ = makeLabel(left, "", theme::kFontCaption, theme::textSecondary());
        status_->setMarginTop(6);
        addView(left);

        // Right column: chips, BIOS banner, facts table, screenshots, description
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(0, 8, 24, 0);

        auto* chips = new brls::Box(brls::Axis::ROW);
        chips->setShrink(0);
        chips->setMarginBottom(10);
        chips->addView(new StatusChip(consoleShortBadge(release_.platform)));
        if (!release_.year.empty() && catalogFoldForSearch(release_.year) != "unknown") {
            const std::string extracted =
                release_.year.size() > 12 ? extractRetroYear(release_.year) : std::string();
            chips->addView(new StatusChip(extracted.empty() ? release_.year : extracted));
        }
        if (!release_.size.empty())
            chips->addView(new StatusChip(release_.size));
        if (stats_.has_value())
            chips->addView(new StatusChip(tr("pipensx/release_chips/peers", stats_->seeds)));
        if (release_.romset)
            chips->addView(new StatusChip(tr("pipensx/retro/romset")));
        else if (!release_.serial.empty())
            chips->addView(new StatusChip(release_.serial));
        content->addView(chips);

        biosSlot_ = new brls::Box(brls::Axis::COLUMN);
        biosSlot_->setShrink(0);
        biosSlot_->setVisibility(brls::Visibility::GONE);
        content->addView(biosSlot_);

        auto* facts = new brls::Box(brls::Axis::COLUMN);
        auto addFact = [facts](const char* key, const std::string& val) {
            if (val.empty())
                return;
            const bool isUnknown = catalogFoldForSearch(val) == "unknown";
            detailFactRow(facts, tr(key), isUnknown ? tr("pipensx/common/unknown") : val);
        };
        addFact("pipensx/detail/fact_genre", release_.genre);
        addFact("pipensx/detail/fact_developer", release_.developer);
        addFact("pipensx/detail/fact_publisher", release_.publisher);
        addFact("pipensx/detail/fact_languages", release_.interfaceLang);
        if (!release_.voiceLang.empty() && release_.voiceLang != release_.interfaceLang)
            addFact("pipensx/retro/voice_lang", release_.voiceLang);
        if (!release_.serial.empty())
            detailFactRow(facts, tr("pipensx/retro/serial"), release_.serial);
        content->addView(facts);

        if (!release_.screenshots.empty() && metadata_) {
            auto* shotSection = new brls::Box(brls::Axis::COLUMN);
            shotSection->setShrink(0);
            shotSection->setVisibility(brls::Visibility::GONE);

            auto* shotHeading = new brls::Label();
            shotHeading->setText(tr("pipensx/retro/screenshots"));
            shotHeading->setFontSize(theme::kFontSmall);
            shotHeading->setTextColor(theme::textSecondary());
            shotHeading->setMarginTop(14);
            shotHeading->setMarginBottom(8);
            shotSection->addView(shotHeading);

            auto* strip = new brls::Box(brls::Axis::ROW);
            strip->setShrink(0);
            strip->setMarginBottom(12);
            const size_t maxThumbs = std::min<size_t>(4, release_.screenshots.size());
            for (size_t i = 0; i < maxThumbs; ++i) {
                auto* btn = new theme::Button();
                btn->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
                btn->setWidth(174);
                btn->setHeight(102);
                btn->setShrink(0);
                btn->setPadding(0, 0, 0, 0);
                btn->setMarginRight(i + 1 == maxThumbs ? 0 : 10);
                btn->setCornerRadius(theme::kRadiusSmall);
                btn->setBackgroundColor(theme::panel());
                btn->setVisibility(brls::Visibility::GONE);
                auto* shot = new AsyncRgbaImage();
                shot->setWidth(174);
                shot->setHeight(102);
                shot->setScalingType(brls::ImageScalingType::FIT);
                shot->setCornerRadius(theme::kRadiusSmall);

                const std::string& shotUrl = release_.screenshots[i];
                if (auto cached = metadata_->cachedImage(shotUrl, GameMetadataService::kImageDimCard);
                    cached && !cached->pixels.empty()) {
                    shotSection->setVisibility(brls::Visibility::VISIBLE);
                    btn->setVisibility(brls::Visibility::VISIBLE);
                    std::shared_ptr<const std::vector<uint8_t>> pixels(cached, &cached->pixels);
                    shot->setRgbaNow(std::move(pixels), cached->width, cached->height);
                } else {
                    shot->setRgbaAsync(
                        [job = job_, metadata = metadata_, shotUrl, shotSection, btn](
                            std::function<void(std::shared_ptr<const std::vector<uint8_t>>, int, int)> done) {
                            metadata->requestImage(
                                shotUrl,
                                [job, shotSection, btn, done](GameMetadataService::ImageData bytes) {
                                    if (!job->alive || !bytes || bytes->pixels.empty()) {
                                        done(nullptr, 0, 0);
                                        return;
                                    }
                                    brls::sync([job, shotSection, btn] {
                                        if (!job->alive)
                                            return;
                                        shotSection->setVisibility(brls::Visibility::VISIBLE);
                                        btn->setVisibility(brls::Visibility::VISIBLE);
                                    });
                                    std::shared_ptr<const std::vector<uint8_t>> pixels(
                                        bytes, &bytes->pixels);
                                    done(std::move(pixels), bytes->width, bytes->height);
                                },
                                GameMetadataService::kImageDimCard,
                                GameMetadataService::ImagePriority::Visible,
                                [job] { return job->alive.load(); });
                        },
                        [job = job_] { return job->alive.load(); });
                }

                btn->addView(shot);
                btn->registerClickAction([this, i](brls::View*) {
                    brls::Application::pushActivity(new ScreenshotViewerActivity(
                        metadata_, release_.screenshots, i, release_.title));
                    return true;
                });
                strip->addView(btn);
            }
            shotSection->addView(strip);
            content->addView(shotSection);
        }

        const auto description = compactDescription(release_.description);
        if (!description.empty()) {
            const auto preview = shortDescription(description);
            auto* desc = makeLabel(content, preview, theme::kFontSmall, theme::textPrimary());
            desc->setMarginTop(10);
            desc->setMarginBottom(0);
            if (preview != description) {
                auto* more = new theme::Button();
                more->setText(tr("pipensx/detail/show_more"));
                more->setFontSize(theme::kFontSmall);
                more->setStyle(&brls::BUTTONSTYLE_DEFAULT);
                more->setHeight(38);
                more->setPadding(0, 14, 0, 14);
                more->setAlignSelf(brls::AlignSelf::FLEX_START);
                more->setMarginTop(8);
                auto expanded = std::make_shared<bool>(false);
                more->registerClickAction([desc, more, content, description, preview, expanded](brls::View*) {
                    *expanded = !*expanded;
                    desc->setText(*expanded ? description : preview);
                    more->setText(tr(*expanded ? "pipensx/detail/show_less" : "pipensx/detail/show_more"));
                    brls::Application::giveFocus(more);
                    if (!*expanded) {
                        if (auto* scroll = dynamic_cast<StickScrollingFrame*>(content->getParent()))
                            scroll->revealFocusAfterLayout();
                    }
                    return true;
                });
                content->addView(more);
            }
        }

        auto* scroll = new StickScrollingFrame();
        scroll->setGrow(1);
        scroll->setShrink(1);
        scroll->setMinWidth(0);
        scroll->setContentView(content);
        addView(scroll);

        refreshLocalUi();
        refreshAsyncState();
    }

    ~RetroDetailView() override {
        job_->alive = false;
        job_->cancelled = true;
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        if (appearedOnce_) {
            if (!busy_)
                status_->setText("");
            refreshLocalUi();
            refreshAsyncState();
        }
        appearedOnce_ = true;
    }

    void draw(NVGcontext* vg, float x, float y, float w, float h, brls::Style style,
              brls::FrameContext* ctx) override {
        const auto now = std::chrono::steady_clock::now();
        if (!busy_ && !refreshInFlight_ &&
            (taskActive_ || autoPreparing_ || (taskCompleted_ && !inLibrary_) || (hasOp_ && !inLibrary_)) &&
            now - lastPoll_ >= std::chrono::milliseconds(750)) {
            lastPoll_ = now;
            refreshAsyncState(false);
        }
        brls::Box::draw(vg, x, y, w, h, style, ctx);
    }

    void showTopicQr() {
        const std::string topicUrl = "https://rutracker.org/forum/viewtopic.php?t=" + release_.topicId;
        auto* box = new brls::Box(brls::Axis::COLUMN);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->setPadding(16, 24, 16, 24);
        auto* title = new brls::Label();
        title->setText(tr("pipensx/detail/topic_action"));
        title->setFontSize(theme::kFontHeading);
        title->setMarginBottom(16);
        box->addView(title);
        auto* qr = new QrCodeView(topicUrl, true);
        qr->setWidth(288);
        qr->setHeight(288);
        qr->setShrink(0);
        box->addView(qr);
        auto* url = new brls::Label();
        url->setText(topicUrl);
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

private:
    void updateSummaryText() {
        std::string line1 = emulatorDisplayName(emulatorId_);
        if (!emulatorId_.empty()) {
            line1 += " · " + tr(emuInstalled_ ? "pipensx/retro/installed_badge"
                                              : "pipensx/retro/auto_install_short");
            if (createForwarder_)
                line1 += " · " + tr("pipensx/retro/home_badge");
        }
        summaryLabel_->setText(line1 + "\nsdmc:/" + folder_);
    }

    void refreshLocalUi() {
        RetroSettings rSettings;
        std::string ignored;
        store_->loadSettings(rSettings, ignored);
        folder_ = rSettings.romDirFor(release_.platform);
        createForwarder_ = rSettings.autoCreateForwarder;
        updateSummaryText();
        const uint64_t needBytes = parseRetroSizeBytes(release_.size);
        const auto storage = queryInstallStorageSpace(
            install::InstallStorageTarget::SdCard, manager_ ? manager_->rootPath() : "sdmc:");
        if (storage.available && storage.totalBytes > 0) {
            if (needBytes > 0)
                meter_->setEstimate(storage.totalBytes, storage.freeBytes, needBytes,
                                    needBytes > storage.freeBytes);
            else
                meter_->setStorage(storage.totalBytes, storage.freeBytes);
        } else {
            meter_->setUnavailable();
        }
    }

    void refreshAsyncState(bool forceRequeue = true) {
        if (refreshInFlight_.exchange(true)) {
            if (forceRequeue)
                refreshQueued_ = true;
            return;
        }
        const auto job = job_;
        const std::string emuId = emulatorId_;
        const std::string opId = release_.platform + "-" + release_.topicId;
        const std::string infoHash = catalogLower(release_.infoHash);
        brls::async([this, job, emuId, opId, infoHash, installer = installer_,
                     store = store_, manager = manager_, plat = release_.platform] {
            std::string path, err;
            RetroSettings setup;
            store->loadSettings(setup, err);
            const bool emuInstalled = !emuId.empty() && installer->detect(setup.detectionFor(plat, emuId), path, err);
            const RetroBiosStatus bios = configuredBiosStatus(setup, plat, emuId);
            RetroRomOperation op;
            const bool hasOp = store->load(opId, op, err);
            std::vector<RetroRomOperation> lib;
            bool inLibrary = false;
            if (store->library(lib, err)) {
                for (const auto& entry : lib) {
                    if (entry.id == opId && !entry.games.empty()) {
                        inLibrary = true;
                        break;
                    }
                }
            }
            const auto task = manager ? manager->snapshotUi(infoHash) : std::nullopt;
            const auto prepSnap = retroAutoPrepareSnapshot(infoHash);
            const bool autoPreparing = prepSnap.active || isRetroTaskAutoPreparing(infoHash);
            brls::sync([this, job, emuId, emuInstalled, bios, hasOp, op, inLibrary, task,
                        autoPreparing, prepSnap, folder = setup.romDirFor(plat),
                        autoFwd = setup.autoCreateForwarder] {
                if (!job->alive)
                    return;
                refreshInFlight_ = false;
                emuInstalled_ = emuInstalled;
                hasOp_ = hasOp;
                op_ = op;
                inLibrary_ = inLibrary;
                autoPreparing_ = autoPreparing;
                folder_ = folder;
                createForwarder_ = autoFwd;
                taskCompleted_ = task && task->status == DownloadStatus::Completed;
                taskActive_ = task.has_value() && !taskCompleted_;

                updateSummaryText();

                if (viewContains(biosSlot_, brls::Application::getCurrentFocus()))
                    brls::Application::giveFocus(primaryBtn_);
                biosSlot_->clearViews();
                if ((bios.required || bios.optional) && !bios.present) {
                    biosSlot_->addView(makeBiosBanner(bios));
                    biosSlot_->setVisibility(brls::Visibility::VISIBLE);
                } else {
                    biosSlot_->setVisibility(brls::Visibility::GONE);
                }

                if (!busy_) {
                    if (inLibrary_) {
                        primaryBtn_->setText(tr("pipensx/retro/in_library_btn"));
                        status_->setText("");
                    } else if (autoPreparing_) {
                        if (prepSnap.creatingForwarder) {
                            primaryBtn_->setText(tr("pipensx/retro/home_working"));
                            status_->setText("");
                        } else if (prepSnap.installingEmulator) {
                            primaryBtn_->setText(tr("pipensx/retro/installing"));
                            status_->setText("");
                        } else if (prepSnap.totalBytes > 0) {
                            const int pct = static_cast<int>(
                                std::min<uint64_t>(100, (prepSnap.doneBytes * 100) / prepSnap.totalBytes));
                            primaryBtn_->setText(tr("pipensx/retro/preparing") + " · " +
                                                 std::to_string(pct) + "%");
                            status_->setText(formatBytes(prepSnap.doneBytes) + " / " +
                                             formatBytes(prepSnap.totalBytes));
                        } else {
                            primaryBtn_->setText(tr("pipensx/retro/preparing"));
                            status_->setText("");
                        }
                    } else if (hasOp_ && op_.ready) {
                        primaryBtn_->setText(tr(op_.games.size() > 1 ? "pipensx/retro/choose_games"
                                                                    : "pipensx/retro/add_library"));
                        status_->setText("");
                    } else if (hasOp_ && taskCompleted_) {
                        primaryBtn_->setText(tr("pipensx/retro/prepare"));
                    } else if (taskActive_) {
                        const uint64_t total = (task && task->wantedTotalBytes > 0)
                            ? task->wantedTotalBytes
                            : (task ? task->totalBytes : 0);
                        const uint64_t done = (task && task->wantedTotalBytes > 0)
                            ? task->wantedCompletedBytes
                            : (task ? task->completedBytes : 0);
                        const int pct = (total > 0)
                            ? static_cast<int>(std::min<uint64_t>(100, (done * 100) / total))
                            : 0;
                        if (task && task->status == DownloadStatus::Downloading && total > 0) {
                            primaryBtn_->setText(tr("pipensx/detail/status_downloading", pct));
                            std::string meta = formatBytes(done) + " / " + formatBytes(total);
                            if (task->speedBytesPerSecond > 0)
                                meta += " · " + formatSpeed(task->speedBytesPerSecond);
                            status_->setText(meta);
                        } else if (task && task->status == DownloadStatus::Paused) {
                            primaryBtn_->setText(pct > 0 ? tr("pipensx/detail/status_paused_percent", pct)
                                                         : tr("pipensx/detail/status_paused"));
                            status_->setText(total > 0 ? (formatBytes(done) + " / " + formatBytes(total)) : "");
                        } else if (task && task->status == DownloadStatus::Error) {
                            primaryBtn_->setText(pct > 0 ? tr("pipensx/detail/status_error_percent", pct)
                                                         : tr("pipensx/detail/status_error"));
                            status_->setText(task->error);
                        } else if (task && (task->status == DownloadStatus::Checking ||
                                            task->status == DownloadStatus::Verifying)) {
                            primaryBtn_->setText(tr("pipensx/detail/status_verifying", pct));
                            status_->setText(total > 0 ? (formatBytes(done) + " / " + formatBytes(total)) : "");
                        } else {
                            primaryBtn_->setText(tr("pipensx/retro/queued_in_downloads"));
                            if (status_->getFullText() == tr("pipensx/retro/files_ready"))
                                status_->setText("");
                        }
                    } else {
                        primaryBtn_->setText(tr("pipensx/common/install"));
                    }
                }
                if (refreshQueued_) {
                    refreshQueued_ = false;
                    refreshAsyncState(false);
                }
            });
        });
    }

    void openInstallOptions() {
        if (busy_)
            return;
        const bool canPickFiles = !inLibrary_ && !taskActive_ && !autoPreparing_ && !(hasOp_ && op_.ready);
        const auto job = job_;
        auto* view = new RetroInstallOptionsView(
            release_.platform, emulatorId_, canPickFiles, store_, installer_,
            [this, job](const std::string& chosenEmulator) {
                if (!job->alive)
                    return;
                emulatorId_ = chosenEmulator;
                refreshLocalUi();
                refreshAsyncState();
            },
            [this, job] {
                if (!job->alive)
                    return;
                startDownloadFlow(true);
            });
        pushDetailActivity(view, tr("pipensx/retro/install_options"));
    }

    void onPrimary() {
        if (busy_)
            return;
        if (inLibrary_) {
            std::vector<RetroRomOperation> entries;
            std::string err;
            const std::string opId = release_.platform + "-" + release_.topicId;
            if (store_->library(entries, err)) {
                for (const auto& e : entries) {
                    if (e.id == opId && !e.games.empty()) {
                        const bool canChooseMore = hasOp_ && op_.ready && op_.games.size() > 1;
                        if (e.games.size() == 1 && !canChooseMore) {
                            pushLibraryGameDetail(e, e.games[0], store_, installer_, manager_,
                                                  metadata_, settings_,
                                                  [this] { refreshAsyncState(); });
                            return;
                        }
                        std::vector<std::string> labels;
                        labels.reserve(e.games.size() + (canChooseMore ? 1 : 0));
                        for (const auto& g : e.games)
                            labels.push_back(gameDisplayTitle(e, g));
                        if (canChooseMore)
                            labels.push_back(tr("pipensx/retro/choose_games"));
                        const auto job = job_;
                        auto* menu = new brls::Dropdown(
                            e.title, labels,
                            [this, job, e, canChooseMore](int index) {
                                if (!job->alive || index < 0)
                                    return;
                                brls::sync([this, job, e, canChooseMore, index] {
                                    if (!job->alive)
                                        return;
                                    const size_t idx = static_cast<size_t>(index);
                                    if (idx < e.games.size()) {
                                        pushLibraryGameDetail(e, e.games[idx], store_, installer_,
                                                              manager_, metadata_, settings_,
                                                              [this] { refreshAsyncState(); });
                                    } else if (canChooseMore) {
                                        pushDetailActivity(
                                            new RetroGamePickerView(
                                                op_, store_,
                                                [this, job](RetroRomOperation updated) {
                                                    if (!job->alive)
                                                        return;
                                                    op_ = std::move(updated);
                                                    refreshAsyncState();
                                                }),
                                            op_.title);
                                    }
                                });
                            });
                        brls::Application::pushActivity(new brls::Activity(menu));
                        return;
                    }
                }
            }
        }
        if (hasOp_ && op_.ready && op_.games.size() > 1) {
            const auto job = job_;
            pushDetailActivity(new RetroGamePickerView(op_, store_, [this, job](RetroRomOperation updated) {
                if (!job->alive)
                    return;
                op_ = std::move(updated);
                refreshAsyncState();
            }), op_.title);
            return;
        }
        if (hasOp_ && op_.ready && op_.games.size() == 1) {
            setBusy(true);
            status_->setText(tr("pipensx/retro/saving_selection"));
            const auto job = job_;
            brls::async([this, job, store = store_, op = op_] {
                std::string err;
                bool ok = true;
                if (!op.games[0].selected)
                    ok = store->select(op.id, {op.games[0].id}, err);
                if (ok)
                    ok = store->addSelectedToLibrary(op.id, err);
                brls::sync([this, job, ok, err] {
                    if (!job->alive)
                        return;
                    setBusy(false);
                    if (ok) {
                        status_->setText(tr("pipensx/retro/library_added"));
                        brls::Application::notify(tr("pipensx/retro/library_added"));
                        refreshAsyncState();
                    } else {
                        status_->setText(localizeRetroError(err));
                    }
                });
            });
            return;
        }
        const std::string infoHash = catalogLower(release_.infoHash);
        if (taskActive_ || autoPreparing_) {
            if (manager_ && manager_->hasTask(infoHash)) {
                brls::Application::pushActivity(
                    new DetailsActivity(infoHash, manager_, nullptr, settings_));
            } else {
                brls::Application::notify(tr(autoPreparing_ ? "pipensx/retro/preparing"
                                                            : "pipensx/retro/wait_download"));
            }
            return;
        }
        if (hasOp_ && taskCompleted_ && !op_.ready) {
            autoPrepareCompletedRetroTask(manager_, metadata_, infoHash);
            if (manager_ && manager_->hasTask(infoHash)) {
                brls::Application::pushActivity(
                    new DetailsActivity(infoHash, manager_, nullptr, settings_));
            } else {
                refreshAsyncState();
            }
            return;
        }
        if (hasOp_ && op_.ready) {
            pushDetailActivity(new OperationView(op_, manager_, store_, installer_, metadata_),
                               op_.title);
            return;
        }
        startDownloadFlow(false);
    }

    void setBusy(bool b) {
        busy_ = b;
        const auto state = b ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED;
        if (b) {
            optionsBtn_->setVisibility(brls::Visibility::GONE);
            cancelBtn_->setVisibility(brls::Visibility::VISIBLE);
            cancelBtn_->setState(brls::ButtonState::ENABLED);
            brls::Application::giveFocus(cancelBtn_);
        } else {
            if (viewContains(cancelBtn_, brls::Application::getCurrentFocus()))
                brls::Application::giveFocus(primaryBtn_);
            cancelBtn_->setVisibility(brls::Visibility::GONE);
            optionsBtn_->setVisibility(brls::Visibility::VISIBLE);
        }
        optionsBtn_->setState(state);
        primaryBtn_->setState(state);
    }

    void startDownloadFlow(bool forcePicker = false, bool biosAcknowledged = false) {
        if (!manager_) {
            status_->setText(tr("pipensx/retro/task_missing"));
            return;
        }
        if (!RetroRomStore::acceptsDestination(folder_)) {
            status_->setText(tr("pipensx/retro/invalid_folder"));
            return;
        }
        const AppSettingsData appCfg = settings_ ? settings_->get() : AppSettingsData{};
        const AddSourceChoice choice = decideAddSource(
            appCfg.torrentingEnabled, appCfg.debridProvider, activeDebridKey(appCfg),
            AddInputKind::Catalog);
        if (!choice.accepted) {
            status_->setText(choice.refusal);
            if (settings_)
                ensureDebridLinked(settings_, manager_);
            return;
        }
        if (manager_->hasTask(release_.infoHash)) {
            status_->setText(tr("pipensx/detail/already_in_downloads"));
            return;
        }
        if (!biosAcknowledged && !emulatorId_.empty()) {
            RetroSettings setup;
            std::string ignoredErr;
            store_->loadSettings(setup, ignoredErr);
            const auto bios = configuredBiosStatus(setup, release_.platform, emulatorId_);
            if (bios.required && !bios.present) {
                const auto job = job_;
                auto* dialog = new brls::Dialog(formatBiosDialogBody(bios));
                dialog->addButton(tr("pipensx/common/cancel"), [] {});
                dialog->addButton(tr("pipensx/common/continue"), [this, job, forcePicker] {
                    brls::sync([this, job, forcePicker] {
                        if (job->alive && !busy_)
                            startDownloadFlow(forcePicker, true);
                    });
                });
                dialog->open();
                return;
            }
        }

        setBusy(true);
        status_->setText(tr("pipensx/retro/resolving"));
        const auto job = job_;
        job->cancelled = false;
        const auto release = release_;
        const auto folder = folder_;
        const auto emuId = emulatorId_;
        const bool createForwarder = !emuId.empty() && createForwarder_;
        const auto emuPackages = catalogEmulators_;
        const auto installer = installer_;
        const std::string appRoot = manager_->rootPath();

        brls::async([this, job, release, folder, emuId, createForwarder, forcePicker,
                     emuPackages, installer, choice, appRoot, store = store_] {
            // Auto-install selected emulator if missing and supported
            if (!job->cancelled && !emuId.empty() && !isCustomEmulatorPath(emuId)) {
                std::string path, err;
                RetroSettings setup;
                store->loadSettings(setup, err);
                if (setup.detectionFor(release.platform, emuId) == emuId && !installer->detect(emuId, path, err)) {
                    const auto resolvedPackages =
                        emuPackages.empty() ? loadCachedEmulatorPackages(appRoot) : emuPackages;
                    const auto pkg = RetroEmulatorInstaller::resolvePackage(emuId, resolvedPackages);
                    if (RetroEmulatorInstaller::supports(pkg)) {
                        brls::sync([this, job, name = pkg.name] {
                            if (job->alive)
                                status_->setText(tr("pipensx/retro/auto_installing_emu", name));
                        });
                        auto lastTick = std::make_shared<std::chrono::steady_clock::time_point>();
                        auto emuProgress = [this, job, name = pkg.name, lastTick](uint64_t done, uint64_t total) {
                            const auto now = std::chrono::steady_clock::now();
                            if (lastTick->time_since_epoch().count() != 0 &&
                                now - *lastTick < std::chrono::milliseconds(250) &&
                                (total == 0 || done < total))
                                return;
                            *lastTick = now;
                            std::string line = tr("pipensx/retro/auto_installing_emu", name);
                            if (total > 0) {
                                const int pct = static_cast<int>(std::min<uint64_t>(100, (done * 100) / total));
                                line += " " + formatBytes(done) + " / " + formatBytes(total) +
                                        " (" + std::to_string(pct) + "%)";
                            } else if (done > 0) {
                                line += " " + formatBytes(done);
                            }
                            brls::sync([this, job, line = std::move(line)] {
                                if (job->alive && busy_)
                                    status_->setText(line);
                            });
                        };
                        std::string emuErr;
                        if (!installer->install(pkg, job->cancelled, emuProgress, emuErr)) {
                            brls::sync([this, job, name = pkg.name, emuErr] {
                                if (job->alive)
                                    brls::Application::notify(tr("pipensx/retro/auto_install_failed_warn",
                                                                 name, localizeRetroError(emuErr)));
                            });
                        }
                    }
                }
            }

            std::string error, path;
            TorrentPreview preview;
            std::vector<uint8_t> peers;
            DebridImport debrid;
            bool ok = !job->cancelled;
            if (ok) {
                static std::atomic<uint32_t> sRetroTempSerial{1};
                std::error_code fsEc;
                std::filesystem::create_directories(appRoot, fsEc);
                path = appRoot + "/_retro_tmp_" + catalogLower(release.infoHash) + "_" +
                       std::to_string(sRetroTempSerial.fetch_add(1)) + ".torrent";
            }
            if (ok && choice.direct) {
                MagnetResolver resolver;
                auto progress = [this, job, last = std::string()](const MagnetProgress& p) mutable {
                    std::string text;
                    switch (p.stage) {
                        case MagnetProgress::Stage::FindingPeers:
                            text = tr("pipensx/detail/finding_peers");
                            break;
                        case MagnetProgress::Stage::Connecting:
                            text = tr("pipensx/detail/contacting_peer", p.peerIndex, p.peerCount);
                            break;
                        case MagnetProgress::Stage::FetchingMetadata:
                            text = tr("pipensx/detail/fetching_metadata",
                                      p.completedPieces, p.totalPieces);
                            break;
                        case MagnetProgress::Stage::Validating:
                            text = tr("pipensx/detail/validating");
                            break;
                    }
                    if (text.empty() || text == last)
                        return;
                    last = text;
                    brls::sync([this, job, text] {
                        if (job->alive && busy_)
                            status_->setText(text);
                    });
                };
                ok = resolver.resolveToFile(release.magnetUri, path, job->cancelled,
                                            progress, error, &peers) &&
                     DownloadManager::previewTorrent(path, preview, error);
                if (ok && catalogLower(preview.infoHash) != catalogLower(release.infoHash)) {
                    ok = false;
                    error = "Retro torrent hash mismatch.";
                }
            } else if (ok) {
                auto provider = makeDebridProvider(choice.provider, choice.credential);
                DebridInfo info;
                std::string id;
                ok = createDebridWithMetainfoFallback(
                    *provider, release.magnetUri, release.infoHash, {}, path, job->cancelled,
                    std::chrono::steady_clock::now() + std::chrono::seconds(90), id, info, error);
                if (ok) {
                    preview = previewFromDebridFiles(info, release.title);
                    preview.infoHash = release.infoHash;
                    debrid.infoHash = release.infoHash;
                    debrid.name = preview.name;
                    debrid.totalBytes = preview.totalBytes;
                    debrid.provider = choice.provider;
                    debrid.debridId = id;
                    debrid.mode = TransferMode::DownloadOnly;
                } else if (!id.empty()) {
                    std::string ignored;
                    provider->remove(id, ignored);
                }
            }

            brls::sync([this, job, choice, ok, error, path, release, folder, emuId,
                        createForwarder, forcePicker,
                        preview = std::move(preview), peers = std::move(peers),
                        debrid = std::move(debrid)]() mutable {
                if (!job->alive || job->cancelled || !ok) {
                    if (!path.empty())
                        unlink(path.c_str());
                    if (!debrid.debridId.empty())
                        removeDebridTransferAsync(choice.provider, choice.credential, debrid.debridId);
                    if (job->alive) {
                        setBusy(false);
                        status_->setText(job->cancelled ? tr("pipensx/retro/cancelled")
                                                        : localizeRetroError(error));
                        refreshAsyncState();
                    }
                    return;
                }
                setBusy(false);
                status_->setText(tr("pipensx/retro/files_ready"));
                refreshAsyncState();

                std::function<void()> abandon;
                if (!debrid.debridId.empty()) {
                    abandon = [choice, id = debrid.debridId] {
                        removeDebridTransferAsync(choice.provider, choice.credential, id);
                    };
                }
                const auto store = store_;
                auto* manager = manager_;
                auto submit = [store, manager, release, folder, emuId, createForwarder, path, peers,
                               debrid](const std::vector<uint8_t>& actions, std::string& error) {
                    if (manager->hasTask(release.infoHash)) {
                        error = tr("pipensx/detail/already_in_downloads");
                        return false;
                    }
                    if (std::any_of(actions.begin(), actions.end(),
                                    [](uint8_t a) { return a > uint8_t(FileAction::Download); })) {
                        error = "Retro accepts download actions only.";
                        return false;
                    }
                    RetroRomOperation existingOp;
                    std::string loadErr;
                    if (store->load(release.platform + "-" + release.topicId, existingOp, loadErr) &&
                        !existingOp.ready) {
                        store->removeOperation(existingOp.id, false, loadErr);
                    }
                    RetroRomOperation operation;
                    if (!store->plan(release, folder, emuId, operation, error, createForwarder))
                        return false;
                    std::string id;
                    if (debrid.debridId.empty()) {
                        return manager->importTorrentActions(path, actions, id, error, peers,
                                                             TransferMode::DownloadOnly, {}, true);
                    }
                    auto import = debrid;
                    import.fileSelection = actions;
                    import.mode = TransferMode::DownloadOnly;
                    import.packageCount = 0;
                    return manager->importDebrid(import, id, error, true);
                };

                std::vector<std::string> previewPaths;
                previewPaths.reserve(preview.files.size());
                for (const auto& f : preview.files)
                    previewPaths.push_back(f.path);
                std::vector<uint8_t> autoActions;
                if (!forcePicker &&
                    planRetroAutoSelection(release.platform, release.romset, previewPaths, autoActions) &&
                    autoActions.size() == preview.files.size()) {
                    uint64_t selectedBytes = 0;
                    for (size_t i = 0; i < preview.files.size(); ++i) {
                        if (autoActions[i] == static_cast<uint8_t>(FileAction::Download))
                            selectedBytes += preview.files[i].length;
                    }
                    const auto storage = pipensx::queryStorageSpace(manager_->rootPath());
                    if (storage.available && selectedBytes > 0 &&
                        storage.freeBytes < selectedBytes) {
                        if (!path.empty())
                            unlink(path.c_str());
                        if (abandon)
                            abandon();
                        status_->setText(tr("pipensx/torrent/no_space"));
                        brls::Application::notify(tr("pipensx/torrent/no_space"));
                        refreshAsyncState();
                        return;
                    }
                    std::string submitErr;
                    if (submit(autoActions, submitErr)) {
                        if (!path.empty())
                            unlink(path.c_str());
                        status_->setText("");
                        brls::Application::notify(tr("pipensx/torrent/added"));
                    } else {
                        if (!path.empty())
                            unlink(path.c_str());
                        if (abandon)
                            abandon();
                        status_->setText(localizeRetroError(submitErr));
                        brls::Application::notify(localizeRetroError(submitErr));
                    }
                    refreshAsyncState();
                    return;
                }
                CatalogEntry retroMeta;
                retroMeta.imageFormat = consoleShortBadge(release.platform);
                if (stats_.has_value()) {
                    retroMeta.hasPeerCount = true;
                    retroMeta.peerCount = static_cast<int>(stats_->seeds);
                }
                brls::Application::pushActivity(new TorrentSelectionActivity(
                    manager_, path, std::move(preview), TransferMode::DownloadOnly,
                    StreamSelection::AllFiles, std::move(peers), std::move(debrid),
                    std::move(abandon), nullptr, retroMeta, std::move(submit)));
            });
        });
    }

    RetroRelease release_;
    std::optional<RetroStats> stats_;
    std::vector<RetroEmulatorPackage> catalogEmulators_;
    DownloadManager* manager_;
    AppSettings* settings_;
    std::shared_ptr<RetroRomStore> store_;
    std::shared_ptr<RetroEmulatorInstaller> installer_;
    GameMetadataService* metadata_;
    std::shared_ptr<Job> job_;

    std::string folder_, emulatorId_;
    bool createForwarder_ = true;
    bool emuInstalled_ = false, hasOp_ = false, inLibrary_ = false;
    bool taskCompleted_ = false, taskActive_ = false, autoPreparing_ = false, busy_ = false;
    bool appearedOnce_ = false, refreshQueued_ = false;
    std::atomic<bool> refreshInFlight_{false};
    std::chrono::steady_clock::time_point lastPoll_{};
    RetroRomOperation op_;

    RetroActionButton *primaryBtn_ = nullptr, *cancelBtn_ = nullptr, *optionsBtn_ = nullptr;
    brls::Label* summaryLabel_ = nullptr;
    StorageMeter* meter_ = nullptr;
    brls::Box* biosSlot_ = nullptr;
    brls::Label* status_ = nullptr;
};

// ============================================================================
// Library Game Detail (multi-console launch, emulator swap, HOME cover art)
// ============================================================================

class LibraryGameView : public brls::Box {
public:
    LibraryGameView(RetroRomOperation entry,
                    RetroRomGame game,
                    std::shared_ptr<RetroRomStore> store,
                    std::shared_ptr<RetroEmulatorInstaller> installer,
                    DownloadManager* manager,
                    GameMetadataService* metadata,
                    AppSettings*,
                    std::function<void()> onChanged)
        : brls::Box(brls::Axis::ROW),
          entry_(std::move(entry)),
          game_(std::move(game)),
          store_(std::move(store)),
          installer_(std::move(installer)),
          manager_(manager),
          metadata_(metadata),
          onChanged_(std::move(onChanged)),
          job_(std::make_shared<Job>()) {
        setGrow(1);
        setPadding(24, 36, 24, 36);

        forwarders_ = makeForwarderService(manager_->rootPath(), store_, installer_, metadata_);

        std::string err;
        store_->loadSettings(settings_, err);

        // Left column: cover + primary launch + HOME actions
        auto* left = new brls::Box(brls::Axis::COLUMN);
        left->setWidth(330);
        left->setShrink(0);
        left->setMarginRight(24);
        left->setClipsToBounds(true);

        auto* coverBox = new brls::Box(brls::Axis::COLUMN);
        coverBox->setHeight(186);
        coverBox->setShrink(0);
        coverBox->setCornerRadius(theme::kRadiusMedium);
        coverBox->setBackgroundColor(theme::panel());
        coverBox->setAlignItems(brls::AlignItems::CENTER);
        coverBox->setJustifyContent(brls::JustifyContent::CENTER);
        coverBox->setPadding(6, 6, 6, 6);
        if (!entry_.coverUrl.empty() && metadata_) {
            auto* cover = new AsyncRgbaImage();
            cover->setWidth(314);
            cover->setHeight(174);
            cover->setScalingType(brls::ImageScalingType::FIT);
            cover->setCornerRadius(theme::kRadiusSmall);
            loadImageInto(cover, metadata_, entry_.coverUrl);
            coverBox->addView(cover);
        } else {
            auto* ph = new brls::Label();
            ph->setText(consoleShortBadge(entry_.platform));
            ph->setFontSize(theme::kFontHeading);
            ph->setTextColor(theme::textSecondary());
            coverBox->addView(ph);
        }
        left->addView(coverBox);

        launch_ = makeButton(left, tr("pipensx/retro/launch_in", ""), [this] { launch(); }, 54.0f);
        launch_->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        launch_->setFontSize(theme::kFontBody);
        launch_->setMarginTop(12);

        changeEmu_ = makeButton(left, tr("pipensx/retro/change_emulator"), [this] { pickEmulator(); }, 46.0f);
        createHome_ = makeButton(left, tr("pipensx/retro/create_home"), [this] { confirmHome(true); }, 46.0f);
        removeHome_ = makeButton(left, tr("pipensx/retro/remove_home"), [this] { confirmHome(false); }, 44.0f);
        remove_ = makeButton(left, tr("pipensx/retro/remove_library"), [this] { confirmRemoveLibrary(); }, 44.0f);
        addView(left);

        // Right column: facts, policy notes, BIOS banner, live status
        auto* right = new brls::Box(brls::Axis::COLUMN);
        right->setPadding(0, 8, 24, 0);

        auto* chips = new brls::Box(brls::Axis::ROW);
        chips->setShrink(0);
        chips->setMarginBottom(12);
        chips->addView(new StatusChip(consoleShortBadge(entry_.platform)));
        if (game_.size > 0)
            chips->addView(new StatusChip(formatBytes(game_.size)));
        if (entry_.imported())
            chips->addView(new StatusChip(tr("pipensx/retro/imported_badge")));
        homeChip_ = new StatusChip(tr("pipensx/retro/home_badge"));
        homeChip_->setVisibility(brls::Visibility::GONE);
        chips->addView(homeChip_);
        right->addView(chips);

        auto* facts = new brls::Box(brls::Axis::COLUMN);
        detailFactRow(facts, tr("pipensx/retro/console"), consoleFullName(entry_.platform));
        emuFact_ = detailFactRowValue(facts, tr("pipensx/retro/emulator"));
        detailFactRow(facts, tr("pipensx/retro/rom_path"),
                      "sdmc:/" + entry_.destination +
                          (entry_.imported() ? "" : "/" + entry_.id) + "/" + game_.path);
        right->addView(facts);

        biosSlot_ = new brls::Box(brls::Axis::COLUMN);
        biosSlot_->setShrink(0);
        right->addView(biosSlot_);

        if (const auto* con = findRetroConsole(entry_.platform);
            con && con->packageInstallInEmulator &&
            catalogLower(baseNameOf(game_.path)) != "eboot.bin") {
            makeLabel(right, tr("pipensx/retro/vita_vpk_note"), theme::kFontSmall,
                      theme::color(theme::Tok::ChipWarning))
                ->setMarginTop(8);
        }

        makeLabel(right, tr("pipensx/retro/launch_policy"), theme::kFontSmall, theme::textSecondary())
            ->setMarginTop(8);
        makeLabel(right, tr("pipensx/retro/home_policy"), theme::kFontSmall, theme::textSecondary());
        status_ = makeLabel(right, "", theme::kFontBody, theme::textPrimary());

        auto* scroll = new StickScrollingFrame();
        scroll->setGrow(1);
        scroll->setShrink(1);
        scroll->setMinWidth(0);
        scroll->setContentView(right);
        addView(scroll);

        refreshState();
    }

    ~LibraryGameView() override {
        job_->alive = false;
        job_->cancelled = true;
    }

private:
    brls::Label* detailFactRowValue(brls::Box* table, const std::string& labelText) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setPadding(6, 0, 6, 0);
        row->setShrink(0);
        auto* name = new brls::Label();
        name->setText(labelText);
        name->setFontSize(theme::kFontSmall);
        name->setTextColor(theme::textSecondary());
        name->setWidth(180);
        name->setShrink(0);
        row->addView(name);
        auto* val = new brls::Label();
        val->setFontSize(theme::kFontSmall);
        val->setTextColor(theme::textPrimary());
        val->setSingleLine(false);
        val->setGrow(1);
        val->setMinWidth(0);
        row->addView(val);
        table->addView(row);
        return val;
    }

    void refreshState() {
        const std::string emuId = resolveEffectiveEmulatorId(settings_, entry_, game_);
        const std::string emuName = emulatorDisplayName(emuId);
        launch_->setText(tr("pipensx/retro/launch_in", emuName));
        if (emuFact_)
            emuFact_->setText(emuName);

        const auto bios = configuredBiosStatus(settings_, entry_.platform, emuId);
        biosSlot_->clearViews();
        if ((bios.required || bios.optional) && !bios.present) {
            biosSlot_->addView(makeBiosBanner(bios));
            biosSlot_->setVisibility(brls::Visibility::VISIBLE);
        } else {
            biosSlot_->setVisibility(brls::Visibility::GONE);
        }

        const auto job = job_;
        brls::async([this, job, forwarders = forwarders_, installer = installer_,
                     detection = settings_.detectionFor(entry_.platform, emuId), gameId = game_.id] {
            const bool hasHome = forwarders->hasShortcut(gameId);
            std::string nro, err;
            const bool hasEmu = installer->detect(detection, nro, err);
            brls::sync([this, job, hasHome, hasEmu] {
                if (!job->alive)
                    return;
                hasHome_ = hasHome;
                hasEmu_ = hasEmu;
                homeChip_->setVisibility(hasHome ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
                createHome_->setText(tr(hasHome ? "pipensx/retro/update_home" : "pipensx/retro/create_home"));
                if (!hasHome && viewContains(removeHome_, brls::Application::getCurrentFocus()))
                    brls::Application::giveFocus(createHome_);
                removeHome_->setVisibility(hasHome ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
                if (!hasEmu && status_->getFullText().empty())
                    status_->setText(tr("pipensx/retro/emulator_missing_hint"));
            });
        });
    }

    void pickEmulator() {
        if (busy_ || pickingEmulator_)
            return;
        pickingEmulator_ = true;
        const auto profiles = emulatorsForConsole(entry_.platform);
        const std::string active = resolveEffectiveEmulatorId(settings_, entry_, game_);
        const auto job = job_;
        brls::async([this, job, profiles, active, installer = installer_] {
            std::vector<std::string> labels;
            std::vector<std::string> ids;
            labels.reserve(profiles.size() + 1);
            ids.reserve(profiles.size() + 1);
            int cur = 0;

            for (size_t i = 0; i < profiles.size(); ++i) {
                std::string path, err;
                const bool installed = installer->detect(profiles[i]->id, path, err);
                std::string lbl = emulatorLabelWithRuntimeTag(*profiles[i]);
                lbl += " · " + tr(installed ? "pipensx/retro/installed_badge"
                                            : "pipensx/retro/auto_install_badge");
                if (profiles[i]->id == active)
                    cur = static_cast<int>(i);
                labels.push_back(std::move(lbl));
                ids.push_back(profiles[i]->id);
            }
            labels.push_back(tr("pipensx/retro/custom_nro") + "...");
            ids.push_back("__custom__");

            brls::sync([this, job, labels = std::move(labels), ids = std::move(ids),
                        cur]() mutable {
                pickingEmulator_ = false;
                if (!job->alive || busy_)
                    return;
                auto* menu = new brls::Dropdown(
                    tr("pipensx/retro/select_emulator"), labels,
                    [this, job, ids = std::move(ids)](int index) {
                        if (!job->alive || index < 0 || static_cast<size_t>(index) >= ids.size())
                            return;
                        const std::string chosen = ids[static_cast<size_t>(index)];
                        if (chosen == "__custom__") {
                            promptCustomNroPath(game_.emulator,
                                                [this, job](const std::string& path) {
                                if (!job->alive)
                                    return;
                                applyEmulatorChoice(path);
                            });
                            return;
                        }
                        applyEmulatorChoice(chosen);
                    },
                    cur);
                brls::Application::pushActivity(new brls::Activity(menu));
            });
        });
    }

    void applyEmulatorChoice(const std::string& chosen) {
        std::string err;
        if (!store_->setGameEmulator(game_.id, chosen, err)) {
            status_->setText(localizeRetroError(err));
            return;
        }
        game_.emulator = chosen;
        refreshState();
        if (hasHome_)
            status_->setText(tr("pipensx/retro/update_home_after_emu"));
        else
            status_->setText(tr("pipensx/retro/emulator_switched", emulatorDisplayName(chosen)));
        if (onChanged_)
            onChanged_();
    }

    void confirmHome(bool create) {
        if (busy_)
            return;
        const auto job = job_;
        auto* dialog = new brls::Dialog(
            tr(create ? "pipensx/retro/create_home_confirm" : "pipensx/retro/remove_home_confirm"));
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->addButton(tr(create ? (hasHome_ ? "pipensx/retro/update_home" : "pipensx/retro/create_home")
                                    : "pipensx/retro/remove_home"),
                          [this, job, create] {
            brls::sync([this, job, create] {
                if (job->alive)
                    home(create);
            });
        });
        dialog->open();
    }

    void home(bool create) {
        if (busy_)
            return;
        setBusy(true);
        status_->setText(tr("pipensx/retro/home_working"));
        const auto job = job_;
        brls::async([this, job, create, service = forwarders_, manager = manager_, id = game_.id] {
            std::string error;
            uint64_t titleId = 0;
            bool ok = manager->beginSystemCleanup(error);
            if (ok) {
                struct Release {
                    DownloadManager* manager;
                    ~Release() { manager->endSystemCleanup(); }
                } release{manager};
                ok = create ? service->create(id, titleId, error) : service->remove(id, error);
            }
            brls::sync([this, job, ok, error, create, titleId] {
                if (!job->alive)
                    return;
                finish(ok ? (create ? tr("pipensx/retro/home_created",
                                         RetroForwarderService::titleIdText(titleId))
                                    : tr("pipensx/retro/home_removed"))
                          : tr("pipensx/retro/home_failed", localizeRetroError(error)));
                refreshState();
                if (onChanged_)
                    onChanged_();
            });
        });
    }

    void confirmRemoveLibrary() {
        if (busy_)
            return;
        std::vector<std::string> options;
        std::vector<std::pair<bool, bool>> actions;
        options.push_back(tr("pipensx/retro/remove_library_only"));
        actions.push_back({false, false});
        if (hasHome_) {
            options.push_back(tr("pipensx/retro/remove_library_home"));
            actions.push_back({false, true});
        }
        if (!entry_.imported()) {
            options.push_back(tr("pipensx/retro/remove_library_files"));
            actions.push_back({true, hasHome_});
        }
        const auto job = job_;
        auto* menu = new brls::Dropdown(
            tr("pipensx/retro/remove_library_confirm"), options,
            [this, job, actions](int index) {
                if (!job->alive || index < 0 || static_cast<size_t>(index) >= actions.size())
                    return;
                const auto choice = actions[static_cast<size_t>(index)];
                brls::sync([this, job, choice] {
                    if (job->alive)
                        executeRemove(choice.first, choice.second);
                });
            });
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    void executeRemove(bool deleteRomFiles, bool removeHomeShortcut) {
        if (busy_)
            return;
        setBusy(true);
        const auto job = job_;
        brls::async([this, job, deleteRomFiles, removeHomeShortcut, store = store_,
                     service = forwarders_, manager = manager_, id = game_.id] {
            std::string error;
            bool ok = true;
            if (removeHomeShortcut) {
                ok = manager->beginSystemCleanup(error);
                if (ok) {
                    struct Release {
                        DownloadManager* manager;
                        ~Release() { manager->endSystemCleanup(); }
                    } release{manager};
                    ok = service->remove(id, error);
                }
            }
            if (ok)
                ok = store->removeFromLibrary(id, deleteRomFiles, error);
            brls::sync([this, job, ok, error] {
                if (!job->alive)
                    return;
                if (ok) {
                    if (onChanged_)
                        onChanged_();
                    brls::Application::popActivity();
                } else {
                    finish(tr("pipensx/retro/home_failed", localizeRetroError(error)));
                }
            });
        });
    }

    void setBusy(bool b) {
        busy_ = b;
        if (b)
            brls::Application::giveFocus(remove_);
        else if (viewContains(remove_, brls::Application::getCurrentFocus()))
            brls::Application::giveFocus(launch_);
        const auto s = b ? brls::ButtonState::DISABLED : brls::ButtonState::ENABLED;
        launch_->setState(s);
        changeEmu_->setState(s);
        createHome_->setState(s);
        removeHome_->setState(s);
        remove_->setState(s, b);
        if (b)
            status_->setText(tr("pipensx/retro/checking_game"));
    }

    void finish(const std::string& message) {
        setBusy(false);
        status_->setText(message);
    }

    void launch(bool biosAcknowledged = false) {
        if (busy_)
            return;
        const std::string emuId = resolveEffectiveEmulatorId(settings_, entry_, game_);
        const auto* profile = findRetroEmulatorProfile(emuId);
        const bool needApp = profile ? profile->applicationModeRequired : true;

#ifdef __SWITCH__
        if (needApp && !isApplicationMode()) {
            status_->setText(tr("pipensx/retro/application_required"));
            return;
        }
        if (!envHasNextLoad()) {
            status_->setText(tr("pipensx/retro/next_load_unavailable"));
            return;
        }
#else
        (void)needApp;
#endif

        if (!biosAcknowledged) {
            const auto bios = configuredBiosStatus(settings_, entry_.platform, emuId);
            if (bios.required && !bios.present) {
                const auto job = job_;
                const std::string pendingPup = (entry_.platform == "psvita")
                    ? findPendingVitaFirmwarePup("sdmc:")
                    : std::string();
                auto* dialog = new brls::Dialog(formatBiosDialogBody(bios));
                if (!pendingPup.empty()) {
                    dialog->addButton(tr("pipensx/retro/vita_install_fw_now"), [this, job, pendingPup] {
                        brls::sync([this, job, pendingPup] {
                            if (!job->alive || busy_)
                                return;
                            std::string nro, args, err;
                            if (!prepareVita3kInstallLaunch(
                                    "sdmc:", pendingPup, "sdmc:/switch/pipensx/pipensx.nro",
                                    nro, args, err)) {
                                finish(localizeRetroError(err));
                                return;
                            }
#ifdef __SWITCH__
                            const Result result = envSetNextLoad(nro.c_str(), args.c_str());
                            if (R_FAILED(result)) {
                                char value[16];
                                std::snprintf(value, sizeof(value), "0x%08x", result);
                                finish(tr("pipensx/retro/launch_failed", value));
                                return;
                            }
                            brls::Application::quit();
#else
                            finish(tr("pipensx/retro/next_load_unavailable"));
#endif
                        });
                    });
                }
                dialog->addButton(tr("pipensx/common/cancel"), [] {});
                dialog->addButton(tr("pipensx/common/continue"), [this, job] {
                    brls::sync([this, job] {
                        if (job->alive && !busy_)
                            launch(true);
                    });
                });
                dialog->open();
                return;
            }
        }

        setBusy(true);
        const auto job = job_;
        brls::async([this, job, store = store_, installer = installer_, id = game_.id, emuId,
                     setup = settings_, platform = entry_.platform] {
            std::string error, romPath, nro, arguments;
            bool ok = store->resolveLibraryGame(id, romPath, error);
            const auto detection = setup.detectionFor(platform, emuId);
            if (ok && !installer->detect(detection, nro, error)) {
                // Attempt auto-install if profile is supported (using cached catalog metadata)
                const auto catalogPkgs = loadCachedEmulatorPackages(store->appRoot());
                const auto pkg = RetroEmulatorInstaller::resolvePackage(emuId, catalogPkgs);
                if (detection == emuId && RetroEmulatorInstaller::supports(pkg)) {
                    brls::sync([this, job, name = pkg.name] {
                        if (job->alive)
                            status_->setText(tr("pipensx/retro/auto_installing_emu", name));
                    });
                    auto lastTick = std::make_shared<std::chrono::steady_clock::time_point>();
                    auto emuProgress = [this, job, name = pkg.name, lastTick](uint64_t done, uint64_t total) {
                        const auto now = std::chrono::steady_clock::now();
                        if (lastTick->time_since_epoch().count() != 0 &&
                            now - *lastTick < std::chrono::milliseconds(250) &&
                            (total == 0 || done < total))
                            return;
                        *lastTick = now;
                        std::string line = tr("pipensx/retro/auto_installing_emu", name);
                        if (total > 0) {
                            const int pct = static_cast<int>(std::min<uint64_t>(100, (done * 100) / total));
                            line += " " + formatBytes(done) + " / " + formatBytes(total) +
                                    " (" + std::to_string(pct) + "%)";
                        } else if (done > 0) {
                            line += " " + formatBytes(done);
                        }
                        brls::sync([this, job, line = std::move(line)] {
                            if (job->alive && busy_)
                                status_->setText(line);
                        });
                    };
                    if (installer->install(pkg, job->cancelled, emuProgress, error))
                        ok = installer->detect(emuId, nro, error);
                    else
                        ok = false;
                } else {
                    ok = false;
                }
            }
            if (ok && platform == "psvita" && emuId == "vita3k") {
                const std::string vitaTid = extractVitaTitleIdFromRomPath(romPath);
                if (vitaTid.empty() || !isVitaTitleInstalledDecrypted("sdmc:", vitaTid)) {
                    const std::string installTarget = resolveVitaInstallTarget(romPath);
                    std::string instNro, instArgs, instErr;
                    if (prepareVita3kInstallLaunch(
                            "sdmc:", installTarget, "sdmc:/switch/pipensx/pipensx.nro",
                            instNro, instArgs, instErr)) {
                        nro = instNro;
                        arguments = instArgs;
                    } else {
                        // First run before .emu/Vita3K.nro is extracted: open Vita3K launcher once.
                        arguments = "\"" + nro + "\"";
                    }
                } else {
                    ok = store->prepareLaunchArguments(setup, platform, emuId, nro, romPath, arguments, error);
                }
            } else if (ok) {
                ok = store->prepareLaunchArguments(setup, platform, emuId, nro, romPath, arguments, error);
            }

            brls::sync([this, job, ok, error, nro, arguments] {
                if (!job->alive)
                    return;
                if (!ok) {
                    finish(localizeRetroError(error));
                    return;
                }
#ifdef __SWITCH__
                const Result result = envSetNextLoad(nro.c_str(), arguments.c_str());
                if (R_FAILED(result)) {
                    char value[16];
                    std::snprintf(value, sizeof(value), "0x%08x", result);
                    finish(tr("pipensx/retro/launch_failed", value));
                    return;
                }
                brls::Application::quit();
#else
                finish(tr("pipensx/retro/next_load_unavailable"));
#endif
            });
        });
    }

    RetroRomOperation entry_;
    RetroRomGame game_;
    std::shared_ptr<RetroRomStore> store_;
    std::shared_ptr<RetroEmulatorInstaller> installer_;
    DownloadManager* manager_;
    GameMetadataService* metadata_;
    std::function<void()> onChanged_;
    std::shared_ptr<RetroForwarderService> forwarders_;
    std::shared_ptr<Job> job_;
    RetroSettings settings_;
    bool busy_ = false, hasHome_ = false, hasEmu_ = false, pickingEmulator_ = false;

    brls::Label *status_ = nullptr, *emuFact_ = nullptr;
    StatusChip* homeChip_ = nullptr;
    brls::Box* biosSlot_ = nullptr;
    RetroActionButton *launch_ = nullptr, *changeEmu_ = nullptr, *createHome_ = nullptr,
                      *removeHome_ = nullptr, *remove_ = nullptr;
};

void pushLibraryGameDetail(RetroRomOperation entry, RetroRomGame game,
                           std::shared_ptr<RetroRomStore> store,
                           std::shared_ptr<RetroEmulatorInstaller> installer,
                           DownloadManager* manager, GameMetadataService* metadata,
                           AppSettings* settings, std::function<void()> onChanged) {
    const std::string title = gameDisplayTitle(entry, game);
    pushDetailActivity(
        new LibraryGameView(std::move(entry), std::move(game), std::move(store),
                            std::move(installer), manager, metadata, settings,
                            std::move(onChanged)),
        title);
}

// ============================================================================
// Virtualized Library list inside My Games -> Retro
// ============================================================================

struct LibraryRowItem {
    RetroRomOperation entry;
    RetroRomGame game;
    std::string activeEmulatorName;
    bool hasHome = false;
};

class RetroLibraryCell : public brls::RecyclerCell {
public:
    RetroLibraryCell() {
        setFocusable(true);
        setHeight(92);
        setPadding(10, 28, 10, 28);
        setCornerRadius(theme::kRadiusSmall);
        setAlignItems(brls::AlignItems::CENTER);
        setAxis(brls::Axis::ROW);

        coverFrame_ = new brls::Box(brls::Axis::COLUMN);
        coverFrame_->setWidth(68);
        coverFrame_->setHeight(68);
        coverFrame_->setShrink(0);
        coverFrame_->setCornerRadius(theme::kRadiusSmall);
        coverFrame_->setBackgroundColor(theme::panel());
        coverFrame_->setAlignItems(brls::AlignItems::CENTER);
        coverFrame_->setJustifyContent(brls::JustifyContent::CENTER);

        cover_ = new AsyncRgbaImage();
        cover_->setWidth(68);
        cover_->setHeight(68);
        cover_->setScalingType(brls::ImageScalingType::FIT);
        cover_->setCornerRadius(theme::kRadiusSmall);
        coverFrame_->addView(cover_);

        badgeFallback_ = new brls::Label();
        badgeFallback_->setFontSize(theme::kFontSmall);
        badgeFallback_->setTextColor(theme::textSecondary());
        badgeFallback_->setSingleLine(true);
        coverFrame_->addView(badgeFallback_);
        addView(coverFrame_);

        auto* mid = new brls::Box(brls::Axis::COLUMN);
        mid->setGrow(1);
        mid->setShrink(1);
        mid->setMinWidth(0);
        mid->setMarginLeft(16);
        mid->setMarginRight(12);

        title_ = new brls::Label();
        title_->setFontSize(theme::kFontBody);
        title_->setTextColor(theme::textPrimary());
        title_->setSingleLine(true);
        mid->addView(title_);

        sub_ = new brls::Label();
        sub_->setFontSize(theme::kFontSmall);
        sub_->setTextColor(theme::textSecondary());
        sub_->setSingleLine(true);
        sub_->setMarginTop(4);
        mid->addView(sub_);
        addView(mid);

        homeChip_ = new StatusChip(tr("pipensx/retro/home_badge"));
        homeChip_->setVisibility(brls::Visibility::GONE);
        addView(homeChip_);
    }

    void bind(const LibraryRowItem& item, GameMetadataService* metadata) {
        if (!item.entry.coverUrl.empty() && metadata) {
            cover_->setVisibility(brls::Visibility::VISIBLE);
            badgeFallback_->setVisibility(brls::Visibility::GONE);
            loadImageInto(cover_, metadata, item.entry.coverUrl);
        } else {
            cover_->setVisibility(brls::Visibility::GONE);
            badgeFallback_->setVisibility(brls::Visibility::VISIBLE);
            badgeFallback_->setText(consoleShortBadge(item.entry.platform));
        }

        title_->setText(gameDisplayTitle(item.entry, item.game));
        std::string sub = consoleShortBadge(item.entry.platform) + " · " + item.activeEmulatorName;
        if (item.game.size > 0)
            sub += " · " + formatBytes(item.game.size);
        if (item.entry.imported())
            sub += " · " + tr("pipensx/retro/imported_badge");
        sub_->setText(sub);
        homeChip_->setVisibility(item.hasHome ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

private:
    brls::Box* coverFrame_ = nullptr;
    AsyncRgbaImage* cover_ = nullptr;
    brls::Label *badgeFallback_ = nullptr, *title_ = nullptr, *sub_ = nullptr;
    StatusChip* homeChip_ = nullptr;
};

class RetroLibraryDataSource : public brls::RecyclerDataSource {
public:
    std::vector<LibraryRowItem> items;
    std::string emptyMessage;
    bool loading = true;
    GameMetadataService* metadata = nullptr;
    std::function<void(const LibraryRowItem&)> onSelect;

    int numberOfRows(brls::RecyclerFrame*, int) override {
        if (items.empty() && loading)
            return 5;
        return std::max<int>(1, static_cast<int>(items.size()));
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath) override {
        return (items.empty() && !loading) ? 140.0f : 92.0f;
    }
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        if (items.empty()) {
            if (loading)
                return recycler->dequeueReusableCell("Skeleton");
            auto* cell = static_cast<TextMessageCell*>(recycler->dequeueReusableCell("Message"));
            cell->setMessage(emptyMessage);
            return cell;
        }
        auto* cell = static_cast<RetroLibraryCell*>(recycler->dequeueReusableCell("Game"));
        cell->bind(items[static_cast<size_t>(index.row)], metadata);
        return cell;
    }
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override {
        if (index.row >= 0 && static_cast<size_t>(index.row) < items.size() && onSelect)
            onSelect(items[static_cast<size_t>(index.row)]);
    }
};

class RetroLibraryView : public brls::Box {
public:
    RetroLibraryView(std::shared_ptr<RetroRomStore> store,
                     std::shared_ptr<RetroEmulatorInstaller> installer,
                     DownloadManager* manager,
                     GameMetadataService* metadata,
                     AppSettings* settings)
        : brls::Box(brls::Axis::COLUMN),
          store_(std::move(store)),
          installer_(std::move(installer)),
          manager_(manager),
          metadata_(metadata),
          settings_(settings),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        setGrow(1);
        forwarders_ = makeForwarderService(manager_->rootPath(), store_, installer_, metadata_);

        auto* bar = new brls::Box(brls::Axis::ROW);
        bar->setShrink(0);
        bar->setPadding(8, 28, 8, 28);
        bar->setAlignItems(brls::AlignItems::CENTER);

        filterBtn_ = catalogChip(tr("pipensx/retro/all_consoles"), [this] { pickConsoleFilter(); });
        filterBtn_->setMarginLeft(0);
        bar->addView(filterBtn_);

        resetBtn_ = catalogChip(tr("pipensx/catalog/filter_reset"), [this] {
            filterConsole_.clear();
            query_.clear();
            applyFilter();
        });
        resetBtn_->setVisibility(brls::Visibility::GONE);
        bar->addView(resetBtn_);

        status_ = new brls::Label();
        status_->setFontSize(theme::kFontSmall);
        status_->setTextColor(theme::textSecondary());
        status_->setSingleLine(true);
        status_->setClipsToBounds(true);
        status_->setGrow(1);
        status_->setShrink(1);
        status_->setMinWidth(0);
        status_->setMarginLeft(14);
        status_->setMarginRight(12);
        bar->addView(status_);

        auto* tools = new brls::Box(brls::Axis::ROW);
        tools->setAlignItems(brls::AlignItems::CENTER);
        tools->setPadding(4, 4, 4, 4);
        tools->setCornerRadius(theme::isCustom() ? theme::kRadiusLarge : 10);
        tools->setBackgroundColor(theme::panel());
        tools->setShrink(0);

        importBtn_ = catalogChip(tr("pipensx/retro/import_folder"), [this] { importFolder(); });
        importBtn_->setMarginLeft(0);
        tools->addView(importBtn_);

        emuBtn_ = catalogChip(tr("pipensx/retro/emulators"), [this] {
            openRetroEmulatorsView(filterConsole_, {}, installer_, store_, [this] { reload(); });
        });
        tools->addView(emuBtn_);
        bar->addView(tools);

        refreshBtn_ = catalogChip(tr("pipensx/installed/refresh_action"), [this] { reload(); });
        bar->addView(refreshBtn_);

        searchBtn_ = new SearchIconButton();
        searchBtn_->setStyle(&brls::BUTTONSTYLE_DEFAULT);
        searchBtn_->setWidth(44);
        searchBtn_->setHeight(44);
        searchBtn_->setShrink(0);
        searchBtn_->setMarginLeft(8);
        searchBtn_->setText("");
        searchBtn_->registerAction("", brls::BUTTON_A, [this](brls::View*) {
            search();
            return true;
        }, true);
        searchBtn_->addGestureRecognizer(new brls::TapGestureRecognizer(searchBtn_));
        bar->addView(searchBtn_);

        addView(bar);

        recycler_ = new SmoothRecyclerFrame();
        recycler_->setGrow(1);
        recycler_->estimatedRowHeight = 92.0f;
        recycler_->registerCell("Game", [] { return new RetroLibraryCell(); });
        recycler_->registerCell("Message", [] { return new TextMessageCell(); });
        recycler_->registerCell("Skeleton", [] { return new SkeletonListRowCell(); });
        data_ = new RetroLibraryDataSource();
        data_->metadata = metadata_;
        data_->emptyMessage = tr("pipensx/retro/library_loading");
        data_->onSelect = [this](const LibraryRowItem& item) {
            pushDetailActivity(
                new LibraryGameView(item.entry, item.game, store_, installer_, manager_,
                                    metadata_, settings_, [this] { reload(); }),
                gameDisplayTitle(item.entry, item.game));
        };
        recycler_->setDataSource(data_);
        host_ = recyclerHost(recycler_);
        addView(host_);

        registerAction(tr("pipensx/common/search"), brls::BUTTON_X, [this](brls::View*) {
            search();
            return true;
        });
    }

    ~RetroLibraryView() override { *alive_ = false; }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        reload();
    }

private:
    void reload() {
        if (busy_)
            return;
        busy_ = true;
        status_->setText(tr("pipensx/retro/library_loading"));
        if (allItems_.empty()) {
            data_->loading = true;
            recycler_->reloadData();
        }
        const auto alive = alive_;
        brls::async([this, alive, store = store_, forwarders = forwarders_] {
            std::vector<RetroRomOperation> entries;
            std::string error;
            const bool ok = store->library(entries, error);
            RetroSettings rSettings;
            std::string ignored;
            store->loadSettings(rSettings, ignored);

            std::vector<LibraryRowItem> loaded;
            if (ok) {
                for (const auto& entry : entries) {
                    for (const auto& game : entry.games) {
                        const std::string effEmu = resolveEffectiveEmulatorId(rSettings, entry, game);
                        const bool hasHome = forwarders->hasShortcut(game.id);
                        loaded.push_back({entry, game, emulatorDisplayName(effEmu), hasHome});
                    }
                }
                std::sort(loaded.begin(), loaded.end(),
                          [](const LibraryRowItem& a, const LibraryRowItem& b) {
                    if (a.entry.platform != b.entry.platform)
                        return a.entry.platform < b.entry.platform;
                    return catalogFoldForSearch(gameDisplayTitle(a.entry, a.game)) <
                           catalogFoldForSearch(gameDisplayTitle(b.entry, b.game));
                });
            }
            brls::sync([this, alive, ok, error, loaded = std::move(loaded)]() mutable {
                if (!*alive)
                    return;
                busy_ = false;
                loadFailed_ = !ok;
                loadError_ = localizeRetroError(error);
                if (ok)
                    allItems_ = std::move(loaded);
                applyFilter();
            });
        });
    }

    EmptyStateView* ensureEmptyState() {
        if (emptyState_)
            return emptyState_;
        emptyState_ = new EmptyStateView();
        addView(emptyState_);
        return emptyState_;
    }

    void applyFilter() {
        brls::View* focused = brls::Application::getCurrentFocus();
        const bool ownsFocus = viewContains(this, focused);
        const bool focusInContent =
            viewContains(host_, focused) ||
            (emptyState_ && viewContains(emptyState_, focused));
        const bool hasFilter = !filterConsole_.empty() || !query_.empty();
        if (!hasFilter && viewContains(resetBtn_, focused))
            brls::Application::giveFocus(filterBtn_);
        resetBtn_->setVisibility(hasFilter ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        filterBtn_->setText(filterConsole_.empty()
                                ? tr("pipensx/retro/all_consoles")
                                : consoleShortBadge(filterConsole_));
        filterBtn_->setStyle(filterConsole_.empty()
                                 ? &brls::BUTTONSTYLE_DEFAULT
                                 : &brls::BUTTONSTYLE_PRIMARY);
        searchBtn_->setStyle(query_.empty()
                                 ? &brls::BUTTONSTYLE_DEFAULT
                                 : &brls::BUTTONSTYLE_PRIMARY);
        searchBtn_->setIconColor(query_.empty()
                                     ? theme::textPrimary()
                                     : theme::onAccent());

        data_->items.clear();
        const std::string q = catalogFoldForSearch(query_);
        for (const auto& item : allItems_) {
            if (!filterConsole_.empty() && item.entry.platform != filterConsole_)
                continue;
            const std::string title = gameDisplayTitle(item.entry, item.game);
            if (!q.empty() &&
                catalogFoldForSearch(title + " " + item.game.path + " " +
                                     consoleShortBadge(item.entry.platform) + " " +
                                     item.activeEmulatorName)
                        .find(q) == std::string::npos)
                continue;
            data_->items.push_back(item);
        }

        if (loadFailed_)
            data_->emptyMessage = loadError_;
        else if (allItems_.empty())
            data_->emptyMessage = tr("pipensx/retro/library_empty");
        else
            data_->emptyMessage = tr("pipensx/retro/no_results");

        data_->loading = busy_ && allItems_.empty();
        recycler_->reloadData();
        const bool empty = data_->items.empty() && !busy_;
        if (empty) {
            if (loadFailed_) {
                ensureEmptyState()->setContent(
                    tr("pipensx/retro/library_empty_title"),
                    loadError_,
                    tr("pipensx/installed/refresh_action"),
                    [this] { reload(); });
            } else if (!allItems_.empty() || hasFilter) {
                ensureEmptyState()->setContent(
                    tr("pipensx/catalog/filtered_empty_title"),
                    tr("pipensx/catalog/filtered_empty_body"),
                    tr("pipensx/catalog/filter_reset"),
                    [this] {
                        filterConsole_.clear();
                        query_.clear();
                        applyFilter();
                    });
            } else {
                ensureEmptyState()->setContent(
                    tr("pipensx/retro/library_empty_title"),
                    tr("pipensx/retro/library_empty"),
                    tr("pipensx/retro/import_folder"),
                    [this] { importFolder(); });
            }
            ensureEmptyState()->setVisibility(brls::Visibility::VISIBLE);
        } else if (emptyState_) {
            emptyState_->setVisibility(brls::Visibility::GONE);
        }
        host_->setVisibility(empty ? brls::Visibility::GONE : brls::Visibility::VISIBLE);

        std::string statusText = loadFailed_
            ? loadError_
            : tr("pipensx/retro/library_count", data_->items.size());
        if (!loadFailed_ && !query_.empty())
            statusText += " · " + query_;
        status_->setText(statusText);

        if (ownsFocus && focusInContent) {
            if (empty)
                brls::Application::giveFocus(ensureEmptyState());
            else
                brls::Application::giveFocus(recycler_);
        }
    }

    void pickConsoleFilter() {
        const auto& consoles = retroConsoles();
        std::vector<std::string> labels{tr("pipensx/retro/all_consoles")};
        int cur = 0;
        for (size_t i = 0; i < consoles.size(); ++i) {
            if (consoles[i].id == filterConsole_)
                cur = static_cast<int>(i + 1);
            labels.push_back("[" + consoles[i].shortBadge + "] " + consoles[i].defaultName);
        }
        const auto alive = alive_;
        auto* menu = new brls::Dropdown(
            tr("pipensx/retro/select_console"), labels,
            [this, alive, consoles](int index) {
                if (!*alive || index < 0)
                    return;
                filterConsole_ =
                    index == 0 ? std::string() : consoles[static_cast<size_t>(index - 1)].id;
                applyFilter();
            },
            cur);
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    void importFolder() {
        const auto& consoles = retroConsoles();
        RetroSettings currentSetup;
        std::string ignoredError;
        store_->loadSettings(currentSetup, ignoredError);
        std::vector<std::string> labels;
        int cur = 0;
        for (size_t i = 0; i < consoles.size(); ++i) {
            const auto& c = consoles[i];
            if (!filterConsole_.empty() && c.id == filterConsole_)
                cur = static_cast<int>(i);
            labels.push_back("[" + c.shortBadge + "] " + c.defaultName +
                             "  (sdmc:/" + currentSetup.romDirFor(c.id) + ")");
        }
        const auto alive = alive_;
        auto* menu = new brls::Dropdown(
            tr("pipensx/retro/import_folder") + " · " + tr("pipensx/retro/select_console"),
            labels,
            [this, alive, consoles](int index) {
                if (!*alive || index < 0 || static_cast<size_t>(index) >= consoles.size())
                    return;
                const auto& chosen = consoles[static_cast<size_t>(index)];
                RetroSettings setup;
                std::string error;
                store_->loadSettings(setup, error);
                const std::string prefill = "sdmc:/" + setup.romDirFor(chosen.id);
                brls::Application::getImeManager()->openForText(
                    [this, alive, consoleId = chosen.id](std::string path) {
                        if (!*alive) return;
                        brls::async([this, alive, consoleId, path, store = store_] {
                            std::vector<RetroRomGame> imported;
                            std::string err;
                            const bool ok = store->importDirectory(consoleId, path, imported, err);
                            brls::sync([this, alive, ok, count = imported.size(), err] {
                                if (!*alive) return;
                                if (!ok) {
                                    brls::Application::notify(localizeRetroError(err));
                                    return;
                                }
                                brls::Application::notify(tr("pipensx/retro/imported_count", count));
                                reload();
                            });
                        });
                    },
                    tr("pipensx/retro/folder"), "", 240, prefill, brls::KEYBOARD_DISABLE_NONE);
            },
            cur);
        brls::Application::pushActivity(new brls::Activity(menu));
    }

    void search() {
        const auto alive = alive_;
        brls::Application::getImeManager()->openForText(
            [this, alive](std::string v) {
                if (!*alive)
                    return;
                query_ = std::move(v);
                applyFilter();
            },
            tr("pipensx/common/search"), "", 128, query_, brls::KEYBOARD_DISABLE_NONE);
    }

    std::shared_ptr<RetroRomStore> store_;
    std::shared_ptr<RetroEmulatorInstaller> installer_;
    DownloadManager* manager_;
    GameMetadataService* metadata_;
    AppSettings* settings_;
    std::shared_ptr<RetroForwarderService> forwarders_;
    std::shared_ptr<std::atomic<bool>> alive_;

    std::vector<LibraryRowItem> allItems_;
    std::string filterConsole_, query_, loadError_;
    bool busy_ = false, loadFailed_ = false;

    theme::Button *filterBtn_ = nullptr, *importBtn_ = nullptr, *emuBtn_ = nullptr,
                  *resetBtn_ = nullptr, *refreshBtn_ = nullptr;
    SearchIconButton* searchBtn_ = nullptr;
    brls::Label* status_ = nullptr;
    EmptyStateView* emptyState_ = nullptr;
    SmoothRecyclerFrame* recycler_ = nullptr;
    brls::Box* host_ = nullptr;
    RetroLibraryDataSource* data_ = nullptr;
};

} // namespace

void openRetroReleaseDetail(
    const RetroRelease& release,
    const RetroStats* stats,
    std::vector<RetroEmulatorPackage> catalogEmulators,
    DownloadManager* manager,
    AppSettings* settings,
    std::shared_ptr<RetroRomStore> store,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    GameMetadataService* metadata) {
    std::optional<RetroStats> statsCopy;
    if (stats)
        statsCopy = *stats;
    auto* view = new RetroDetailView(
        release, statsCopy, std::move(catalogEmulators), manager, settings,
        std::move(store), std::move(installer), metadata);
    auto* frame = pushDetailActivity(view, release.title);
    frame->registerAction(tr("pipensx/detail/topic_action"), brls::BUTTON_BACK,
                          [view](brls::View*) {
                              view->showTopicQr();
                              return true;
                          });
}

void openRetroEmulatorsView(
    std::string initialConsoleId,
    std::vector<RetroEmulatorPackage> catalogEmulators,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    std::shared_ptr<RetroRomStore> store,
    std::function<void()> onChanged) {
    const std::string title = initialConsoleId.empty()
        ? tr("pipensx/retro/emulators")
        : tr("pipensx/retro/emulators") + " · " + consoleFullName(initialConsoleId);
    auto* view = new RetroEmulatorsView(
        std::move(initialConsoleId), std::move(catalogEmulators),
        std::move(installer), std::move(store), std::move(onChanged));
    pushDetailActivity(view, title);
}

brls::View* retroLibraryView(
    std::shared_ptr<RetroRomStore> store,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    DownloadManager* manager,
    GameMetadataService* metadata,
    AppSettings* settings) {
    return new RetroLibraryView(std::move(store), std::move(installer), manager, metadata, settings);
}

void showRetroOperations(
    DownloadManager* manager,
    std::shared_ptr<RetroRomStore> store,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    GameMetadataService* metadata,
    std::shared_ptr<std::atomic<bool>> alive) {
    brls::async([manager, store, installer, metadata, alive] {
        std::vector<RetroRomOperation> operations;
        std::string error;
        const bool ok = store->list(operations, error);
        brls::sync([manager, store, installer, metadata, alive, ok, error,
                    operations = std::move(operations)] {
            if (!*alive)
                return;
            if (!ok) {
                brls::Application::notify(localizeRetroError(error));
                return;
            }
            if (operations.empty()) {
                brls::Application::notify(tr("pipensx/retro/no_operations"));
                return;
            }
            std::vector<std::string> labels;
            for (const auto& op : operations) {
                std::string label = "[" + consoleShortBadge(op.platform) + "] " + op.title;
                if (op.ready)
                    label += " (" + std::to_string(op.games.size()) + ")";
                labels.push_back(std::move(label));
            }
            auto* menu = new brls::Dropdown(
                tr("pipensx/retro/operations"), labels,
                [manager, store, installer, metadata, alive, operations](int index) {
                    if (!*alive || index < 0 || size_t(index) >= operations.size())
                        return;
                    brls::sync([manager, store, installer, metadata, alive, op = operations[index]] {
                        if (!*alive)
                            return;
                        pushDetailActivity(
                            new OperationView(op, manager, store, installer, metadata),
                            op.title);
                    });
                });
            brls::Application::pushActivity(new brls::Activity(menu));
        });
    });
}

void autoPrepareCompletedRetroTask(DownloadManager* manager, GameMetadataService* metadata,
                                   const std::string& taskId) {
    if (!manager || taskId.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(gAutoPrepMutex);
        if (!gAutoPrepActive.insert(taskId).second)
            return;
    }
    setRetroAutoPrepareProgress(taskId, true);

    const std::string appRoot = manager->rootPath();
    brls::async([manager, metadata, appRoot, taskId] {
        struct Guard {
            std::string id;
            ~Guard() {
                setRetroAutoPrepareProgress(id, false);
                std::lock_guard<std::mutex> lock(gAutoPrepMutex);
                gAutoPrepActive.erase(id);
            }
        } guard{taskId};

        auto store = std::make_shared<RetroRomStore>(appRoot, "sdmc:");
        RetroRomOperation op;
        std::string error;
        if (!store->findOperationByTask(taskId, op, error) || op.ready)
            return;

        auto lease = manager->beginExternalDeploy(taskId, error);
        if (!lease || lease->task().mode != TransferMode::DownloadOnly)
            return;

        TaskFileInventory inventory;
        if (!buildTaskFileInventory(appRoot, lease->task(), inventory, error) ||
            !inventory.completeManifest)
            return;

        std::vector<RetroRomInput> files;
        uint64_t totalInputBytes = 0;
        for (const auto& file : inventory.files) {
            if (file.action != TaskFileAction::Skip) {
                if (file.state != TaskFileState::Present) {
                    error = "Selected download file is missing or unsafe.";
                    brls::sync([error] {
                        brls::Application::notify(
                            tr("pipensx/retro/preparation_failed", localizeRetroError(error)));
                    });
                    return;
                }
                files.push_back({file.logicalPath, file.absolutePath, file.size});
                totalInputBytes += file.size;
            }
        }
        if (files.empty())
            return;

        setRetroAutoPrepareProgress(taskId, true, 0, totalInputBytes, 0, files.size());

        std::atomic<bool> cancelled{false};
        RetroRomOperation prepared;
        auto lastTick = std::make_shared<std::chrono::steady_clock::time_point>();
        auto prepProgress = [taskId, lastTick](uint64_t doneBytes, uint64_t totalBytes,
                                               size_t doneFiles, size_t totalFiles) {
            const auto now = std::chrono::steady_clock::now();
            if (lastTick->time_since_epoch().count() != 0 &&
                now - *lastTick < std::chrono::milliseconds(100) &&
                (totalBytes == 0 || doneBytes < totalBytes))
                return;
            *lastTick = now;
            setRetroAutoPrepareProgress(taskId, true, doneBytes, totalBytes, doneFiles, totalFiles);
        };
        if (!store->prepare(op.id, files, cancelled, prepared, error, prepProgress)) {
            brls::sync([error] {
                brls::Application::notify(
                    tr("pipensx/retro/preparation_failed", localizeRetroError(error)));
            });
            return;
        }

        if (prepared.games.size() == 1) {
            std::string addErr;
            const bool added = store->addSelectedToLibrary(prepared.id, addErr);

            RetroSettings setup;
            std::string settingsErr;
            store->loadSettings(setup, settingsErr);
            const std::string emuId = resolveEffectiveEmulatorId(setup, prepared, prepared.games[0]);
            auto installer = std::make_shared<RetroEmulatorInstaller>("sdmc:");

            // Ensure selected emulator is installed if missing and supported.
            if (added && !emuId.empty() && !isCustomEmulatorPath(emuId)) {
                std::string nroPath, detErr;
                if (setup.detectionFor(prepared.platform, emuId) == emuId &&
                    !installer->detect(emuId, nroPath, detErr)) {
                    const auto pkgs = loadCachedEmulatorPackages(appRoot);
                    const auto pkg = RetroEmulatorInstaller::resolvePackage(emuId, pkgs);
                    if (RetroEmulatorInstaller::supports(pkg)) {
                        setRetroAutoPrepareProgress(taskId, true, totalInputBytes, totalInputBytes,
                                                    files.size(), files.size(), true, false);
                        std::string instErr;
                        installer->install(pkg, cancelled, {}, instErr);
                    }
                }
            }

            // Automatically create HOME forwarder while holding deploy lease (blocks concurrent NCM).
            bool forwarderRequested = added && prepared.createForwarder && !emuId.empty();
            bool forwarderCreated = false;
            std::string forwarderError;
            if (forwarderRequested) {
                setRetroAutoPrepareProgress(taskId, true, totalInputBytes, totalInputBytes,
                                            files.size(), files.size(), false, true);
                auto forwarders = makeForwarderService(appRoot, store, installer, metadata);
                uint64_t titleId = 0;
                forwarderCreated = forwarders->create(prepared.games[0].id, titleId, forwarderError);
            }

            const RetroBiosStatus bios = configuredBiosStatus(setup, prepared.platform, emuId);
            const auto* console = findRetroConsole(prepared.platform);
            std::string vitaInstallTarget;
            std::string pendingVitaPup;
            bool needsVitaInstall = false;
            if (prepared.platform == "psvita") {
                std::string resolvedRom, resolveErr;
                if (store->resolveLibraryGame(prepared.games[0].id, resolvedRom, resolveErr)) {
                    const std::string vitaTid = extractVitaTitleIdFromRomPath(resolvedRom);
                    needsVitaInstall = vitaTid.empty() || !isVitaTitleInstalledDecrypted("sdmc:", vitaTid);
                    if (needsVitaInstall)
                        vitaInstallTarget = resolveVitaInstallTarget(resolvedRom);
                }
                if (!bios.present)
                    pendingVitaPup = findPendingVitaFirmwarePup("sdmc:");
            }
            const bool needsInEmuInstall =
                console && console->packageInstallInEmulator && prepared.platform != "psvita";

            // Release deploy lease and remove raw download copy so SD space isn't doubled.
            lease.reset();
            {
                std::string rmErr;
                manager->remove(taskId, true, rmErr);
            }

            if (added) {
                const bool showDialog = (bios.required && !bios.present) ||
                                        needsInEmuInstall ||
                                        needsVitaInstall ||
                                        (forwarderRequested && !forwarderCreated);
                brls::sync([title = prepared.title, forwarderRequested, forwarderCreated,
                            forwarderError, bios, needsInEmuInstall, needsVitaInstall,
                            vitaInstallTarget, pendingVitaPup, showDialog] {
                    if (!showDialog) {
                        std::string msg = forwarderCreated
                            ? tr("pipensx/retro/notify_installed_home", title)
                            : tr("pipensx/retro/notify_ready", title);
                        if (bios.optional && !bios.present) {
                            const std::string hint = bios.hintKey.empty()
                                ? tr("pipensx/retro/bios_generic", bios.expectedPath)
                                : brls::getStr(bios.hintKey.c_str(), bios.expectedPath);
                            msg += " " + hint;
                        }
                        brls::Application::notify(msg);
                        return;
                    }

                    std::string body = forwarderCreated
                        ? tr("pipensx/retro/notify_installed_home", title)
                        : tr("pipensx/retro/notify_ready", title);
                    if (forwarderRequested && !forwarderCreated) {
                        body += "\n\n" + tr("pipensx/retro/auto_home_failed",
                                            localizeRetroError(forwarderError));
                    }
                    if (needsVitaInstall) {
                        body += "\n\n" + tr("pipensx/retro/vita_install_prompt");
                    } else if (needsInEmuInstall) {
                        body += "\n\n" + tr("pipensx/retro/vita_vpk_note");
                    }
                    if ((bios.required || bios.optional) && !bios.present) {
                        const std::string hint = bios.hintKey.empty()
                            ? tr("pipensx/retro/bios_generic", bios.expectedPath)
                            : brls::getStr(bios.hintKey.c_str(), bios.expectedPath);
                        body += "\n\n" + tr(bios.required ? "pipensx/retro/bios_required_title"
                                                          : "pipensx/retro/bios_optional_title") +
                                ":\n" + hint;
                    }
                    auto* dialog = new brls::Dialog(body);
                    auto launchVitaInstaller = [](const std::string& target) {
                        std::string nro, args, err;
                        if (!prepareVita3kInstallLaunch(
                                "sdmc:", target, "sdmc:/switch/pipensx/pipensx.nro",
                                nro, args, err)) {
                            brls::Application::notify(localizeRetroError(err));
                            return;
                        }
#ifdef __SWITCH__
                        if (!envHasNextLoad()) {
                            brls::Application::notify(tr("pipensx/retro/next_load_unavailable"));
                            return;
                        }
                        const Result result = envSetNextLoad(nro.c_str(), args.c_str());
                        if (R_FAILED(result)) {
                            char value[16];
                            std::snprintf(value, sizeof(value), "0x%08x", result);
                            brls::Application::notify(tr("pipensx/retro/launch_failed", value));
                            return;
                        }
                        brls::Application::quit();
#else
                        brls::Application::notify(tr("pipensx/retro/next_load_unavailable"));
#endif
                    };
                    if (!pendingVitaPup.empty()) {
                        dialog->addButton(tr("pipensx/retro/vita_install_fw_now"),
                                          [launchVitaInstaller, pendingVitaPup] {
                            launchVitaInstaller(pendingVitaPup);
                        });
                    }
                    if (needsVitaInstall && !vitaInstallTarget.empty()) {
                        dialog->addButton(tr("pipensx/retro/vita_install_now"),
                                          [launchVitaInstaller, vitaInstallTarget] {
                            launchVitaInstaller(vitaInstallTarget);
                        });
                    }
                    dialog->addButton(tr("pipensx/common/ok"), [] {});
                    dialog->open();
                });
                return;
            }
        }

        // Release deploy lease and remove raw download copy so SD space isn't doubled.
        lease.reset();
        {
            std::string rmErr;
            manager->remove(taskId, true, rmErr);
        }

        brls::sync([title = prepared.title, count = prepared.games.size()] {
            brls::Application::notify(tr("pipensx/retro/notify_romset_ready", title, count));
        });
    });
}

void syncExistingRetroLibrary(DownloadManager* manager, GameMetadataService* metadata) {
    if (!manager)
        return;
    const std::string appRoot = manager->rootPath();
    brls::async([manager, metadata, appRoot] {
        auto store = std::make_shared<RetroRomStore>(appRoot, "sdmc:");
        auto installer = std::make_shared<RetroEmulatorInstaller>("sdmc:");
        auto forwarders = makeForwarderService(appRoot, store, installer, metadata);
        std::vector<RetroRomOperation> entries;
        std::string err;
        if (!store->library(entries, err))
            return;
        for (const auto& entry : entries) {
            if (entry.platform != "psvita")
                continue;
            for (const auto& g : entry.games) {
                std::string romPath, resolveErr;
                if (!store->resolveLibraryGame(g.id, romPath, resolveErr))
                    continue;
                if (forwarders->hasShortcut(g.id)) {
                    std::string cleanErr;
                    if (manager->beginSystemCleanup(cleanErr)) {
                        struct Release {
                            DownloadManager* m;
                            ~Release() { m->endSystemCleanup(); }
                        } release{manager};
                        uint64_t tid = 0;
                        std::string fwdErr;
                        forwarders->create(g.id, tid, fwdErr);
                    }
                }
            }
        }
    });
}

} // namespace pipensx::ui
