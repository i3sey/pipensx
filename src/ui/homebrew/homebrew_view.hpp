#pragma once

#include <borealis.hpp>
#include "app/app_settings.hpp"
#include "app/game_metadata_service.hpp"
#include "app/homebrew_catalog.hpp"
#include "ui/common/smooth_recycler.hpp"
#include "ui/theme.hpp"
#include <memory>

namespace pipensx::ui {

class HomebrewDataSource;

class HomebrewView : public brls::Box {
public:
    HomebrewView(std::string appRoot, std::string sdRoot, AppSettings* settings,
                 GameMetadataService* metadata, bool installedOnly = false);
    ~HomebrewView() override;
    void willAppear(bool resetState) override;

private:
    void update(bool refresh);
    void rebuild();
    void search();
    void open(const HomebrewPackage& package);
    std::shared_ptr<HomebrewCatalog> service_;
    AppSettings* settings_;
    GameMetadataService* metadata_;
    std::shared_ptr<std::atomic<bool>> alive_, cancelled_;
    std::vector<HomebrewPackage> packages_;
    std::string query_;
    bool busy_ = false, initialized_ = false;
    bool installedOnly_ = false;
    brls::Label* status_;
    theme::Button *search_ = nullptr, *reset_ = nullptr;
    SmoothRecyclerFrame* recycler_;
    HomebrewDataSource* data_;
};

} // namespace pipensx::ui
