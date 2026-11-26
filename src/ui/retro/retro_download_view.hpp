#pragma once

#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/retro_catalog.hpp"
#include "app/retro_emulator.hpp"
#include "app/retro_rom.hpp"
#include <borealis.hpp>
#include <functional>
#include <vector>

namespace pipensx::ui {

void openRetroReleaseDetail(
    const RetroRelease& release,
    const RetroStats* stats,
    std::vector<RetroEmulatorPackage> catalogEmulators,
    DownloadManager* manager,
    AppSettings* settings,
    std::shared_ptr<RetroRomStore> store,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    GameMetadataService* metadata);

void openRetroEmulatorsView(
    std::string initialConsoleId,
    std::vector<RetroEmulatorPackage> catalogEmulators,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    std::shared_ptr<RetroRomStore> store,
    std::function<void()> onChanged = {});

brls::View* retroLibraryView(
    std::shared_ptr<RetroRomStore> store,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    DownloadManager* manager,
    GameMetadataService* metadata = nullptr,
    AppSettings* settings = nullptr);

void showRetroOperations(
    DownloadManager* manager,
    std::shared_ptr<RetroRomStore> store,
    std::shared_ptr<RetroEmulatorInstaller> installer,
    GameMetadataService* metadata,
    std::shared_ptr<std::atomic<bool>> alive);

void autoPrepareCompletedRetroTask(
    DownloadManager* manager,
    GameMetadataService* metadata,
    const std::string& taskId);

void syncExistingRetroLibrary(
    DownloadManager* manager,
    GameMetadataService* metadata);

} // namespace pipensx::ui
