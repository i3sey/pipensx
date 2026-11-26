#pragma once

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/retro_catalog.hpp"
#include "app/retro_emulator.hpp"
#include "app/retro_rom.hpp"
#include "ui/common/smooth_recycler.hpp"
#include "ui/theme.hpp"
#include <borealis.hpp>

namespace pipensx::ui {

class RetroDataSource;

class RetroView : public brls::Box {
public:
    RetroView(std::string appRoot, std::string sdRoot, AppSettings* settings,
              GameMetadataService* metadata, DownloadManager* manager);
    ~RetroView() override;
    void willAppear(bool resetState) override;

private:
    enum class SortMode {
        SeedsDesc = 0,
        TitleAsc = 1,
        YearDesc = 2,
        SizeDesc = 3,
    };
    enum class TypeFilter {
        All = 0,
        GamesOnly = 1,
        RomsetsOnly = 2,
    };

    void update(bool forceShared = false, bool forceNetwork = false);
    void rebuild(bool scheduleOnly = false);
    void applyFiltered(std::vector<RetroRelease> filtered, uint64_t gen);
    void search();
    void pickConsole();
    void openSortSheet();
    void setSort(SortMode mode);
    void openTypeSheet();
    void setTypeFilter(TypeFilter filter);
    static bool naturalDescending(SortMode mode);
    static std::string sortLabel(SortMode mode);
    static std::string typeFilterLabel(TypeFilter filter);
    void openRelease(const RetroRelease& release);
    void openEmulators();
    std::string consoleId() const;

    std::shared_ptr<RetroCatalog> catalog_;
    std::shared_ptr<RetroEmulatorInstaller> installer_;
    std::shared_ptr<RetroRomStore> roms_;
    DownloadManager* manager_ = nullptr;
    AppSettings* settings_ = nullptr;
    GameMetadataService* metadata_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_, cancelled_;

    RetroSettings retroSettings_;
    std::string activeConsole_ = "gba";
    RetroCatalogSnapshot snapshot_;
    bool sharedLoaded_ = false;
    std::string query_, failure_;
    SortMode sortMode_ = SortMode::SeedsDesc;
    bool sortReversed_ = false;
    TypeFilter typeFilter_ = TypeFilter::All;
    bool busy_ = false, initialized_ = false, cached_ = false, filtering_ = false;
    std::atomic<uint64_t> fetchGeneration_{0};
    std::atomic<uint64_t> filterGeneration_{0};

    brls::Label* status_ = nullptr;
    theme::Button *consoleBtn_ = nullptr, *sortBtn_ = nullptr, *typeBtn_ = nullptr,
                  *emuBtn_ = nullptr, *refresh_ = nullptr,
                  *search_ = nullptr, *reset_ = nullptr;
    brls::Box* recyclerHost_ = nullptr;
    SmoothRecyclerFrame* recycler_ = nullptr;
    RetroDataSource* data_ = nullptr;
};

} // namespace pipensx::ui
