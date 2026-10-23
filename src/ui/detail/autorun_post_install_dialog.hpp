#pragma once

#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/autorun_service.hpp"
#include "app/download_manager.hpp"
#include "install/install_backend.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

inline void openAutorunPostInstallDialog(
    const std::string& taskId,
    DownloadManager& manager,
    AutorunService* autorun,
    std::function<void()> onDismiss) {

    const auto task = manager.snapshot(taskId);
    AutorunProfile profile;
    std::string profileError;
    loadAutorunProfile(manager.rootPath(), taskId, profile, profileError);

    std::string gameTitle = !profile.title.empty()
        ? profile.title
        : (task ? task->name : "Game");
    std::string gameFolder = !profile.targetFolder.empty()
        ? profile.targetFolder
        : (task ? task->dataPath : gameTitle);
    const std::string sdRoot = autorun ? autorun->sdRoot() : "sdmc:";
    std::string executable = resolveAutorunExecutable(sdRoot, gameFolder, profile.executable);

    class Body : public brls::Box {
    public:
        std::function<void()> onDismiss;
        ~Body() override {
            if (onDismiss)
                onDismiss();
        }
    };

    auto* box = new Body();
    box->setAxis(brls::Axis::COLUMN);
    box->setPadding(
        brls::Application::getStyle()["brls/dialog/paddingTopBottom"],
        brls::Application::getStyle()["brls/dialog/paddingLeftRight"],
        brls::Application::getStyle()["brls/dialog/paddingTopBottom"],
        brls::Application::getStyle()["brls/dialog/paddingLeftRight"]);

    auto* titleLabel = new brls::Label();
    titleLabel->setFontSize(brls::Application::getStyle()["brls/dialog/fontSize"]);
    titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    titleLabel->setSingleLine(false);
    titleLabel->setText(tr("pipensx/autorun/ready_to_launch_title", gameTitle));
    box->addView(titleLabel);

    auto* descLabel = new brls::Label();
    descLabel->setFontSize(theme::kFontSmall);
    descLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    descLabel->setSingleLine(false);
    descLabel->setTextColor(theme::textSecondary());
    descLabel->setMarginTop(8);
    descLabel->setMarginBottom(16);
    descLabel->setText(tr("pipensx/autorun/ready_to_launch_desc"));
    box->addView(descLabel);

    auto wantForwarder = std::make_shared<bool>(true);
    auto wantSysclk = std::make_shared<bool>(true);

    auto* forwarderCell = new brls::BooleanCell();
    forwarderCell->init(tr("pipensx/autorun/option_create_forwarder"), true,
                        [wantForwarder](bool val) { *wantForwarder = val; });
    forwarderCell->setMarginBottom(8);
    box->addView(forwarderCell);

    auto* sysclkCell = new brls::BooleanCell();
    sysclkCell->init(tr("pipensx/autorun/option_apply_sysclk"), true,
                     [wantSysclk](bool val) { *wantSysclk = val; });
    sysclkCell->setMarginBottom(8);
    box->addView(sysclkCell);

    auto dismissed = std::make_shared<std::atomic<bool>>(false);
    auto onDismissShared = std::make_shared<std::function<void()>>(std::move(onDismiss));

    box->onDismiss = [dismissed, onDismissShared] {
        if (!dismissed->exchange(true) && *onDismissShared) {
            (*onDismissShared)();
        }
    };

    auto* dialog = new brls::Dialog(box);

    dialog->addButton(tr("pipensx/common/continue"),
                      [dismissed, onDismissShared, &manager, autorun,
                       wantForwarder, wantSysclk, gameTitle, gameFolder,
                       executable, profile]() {
        const bool doForwarder = *wantForwarder;
        const bool doSysclk = *wantSysclk;
        const std::string sdRoot = autorun ? autorun->sdRoot() : "sdmc:";

        // Guarantee registration in AutoRun library and target.txt
        const std::string wineRoot = autorunWineRoot(sdRoot);
        std::string fullTarget = "sdmc:/switch/wine/drive_c/Games/" + gameFolder + "/" + executable;
        std::replace(fullTarget.begin(), fullTarget.end(), '\\', '/');
        const std::string coverTarget = "sdmc:/switch/wine/drive_c/Games/" + gameFolder + "/cover.png";
        registerGameInLauncherLibrary(sdRoot, fullTarget, coverTarget);

        const std::string targetTxtPath = wineRoot + "/target.txt";
        std::ofstream targetOut(targetTxtPath, std::ios::binary | std::ios::trunc);
        if (targetOut) {
            targetOut << fullTarget << "\n";
        }

        if (doForwarder && !executable.empty()) {
            auto backend = install::createInstallBackend(
                manager.rootPath(), install::InstallStorageTarget::SdCard);
            std::string forwarderErr;
            const std::vector<uint8_t> iconJpeg;
            if (backend && installAutorunForwarder(sdRoot, *backend, gameTitle,
                                                  gameFolder, executable,
                                                  iconJpeg, forwarderErr)) {
                brls::Application::notify(tr("pipensx/autorun/forwarder_installed"));
            } else {
                brls::Application::notify(
                    tr("pipensx/autorun/forwarder_failed", forwarderErr));
            }
        }

        if (doSysclk && !profile.overclock.empty()) {
            std::string sysclkErr;
            const uint64_t titleId = generateAutorunTitleId(gameTitle);
            SysClkProfile sysProfile;
            parseOverclockProfile(profile.overclock, sysProfile);
            if (applySysClkProfile(sdRoot, titleId, sysProfile, sysclkErr)) {
                brls::Application::notify(tr("pipensx/autorun/sysclk_applied"));
            } else {
                brls::Application::notify(
                    tr("pipensx/autorun/sysclk_failed", sysclkErr));
            }
        }

        if (!dismissed->exchange(true) && *onDismissShared) {
            (*onDismissShared)();
        }
    });

    dialog->addButton(tr("pipensx/common/skip"), [dismissed, onDismissShared]() {
        if (!dismissed->exchange(true) && *onDismissShared) {
            (*onDismissShared)();
        }
    });

    dialog->open();
}

} // namespace pipensx::ui
