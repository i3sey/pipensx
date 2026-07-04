#pragma once

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

#include <borealis.hpp>

extern "C" {
#include "core/util.h"
}
#include "app/app_settings.hpp"
#include "app/download_manager.hpp"
#include "install/install_backend.hpp"
#include "platform/switch_crashlog.h"

namespace pipensx::ui {

inline pipensx::install::InstallStorageTarget installTargetFor(InstallLocation value) {
    return value == InstallLocation::SystemMemory
               ? pipensx::install::InstallStorageTarget::Nand
               : pipensx::install::InstallStorageTarget::SdCard;
}

inline FILE* gBorealisLog = nullptr;
inline std::atomic<uint32_t> gCatalogTempSerial{0};
inline constexpr const char* TelemetryFlagPath =
    "sdmc:/switch/pipensx/throughput_telemetry.enabled";
inline constexpr const char* SettingsPath =
    "sdmc:/switch/pipensx/settings.json";
inline constexpr const char* LogPath =
    "sdmc:/switch/pipensx/pipensx.log";

inline void openBorealisLog() {
    if (gBorealisLog)
        return;
    gBorealisLog = std::fopen(LogPath, "a");
    if (gBorealisLog) {
        brls::Logger::setLogOutput(gBorealisLog);
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }
}

inline bool clearApplicationLog() {
    if (gBorealisLog) {
        std::fflush(gBorealisLog);
        brls::Logger::setLogOutput(nullptr);
        std::fclose(gBorealisLog);
        gBorealisLog = nullptr;
    }
    bool ok = log_clear() != 0;
    openBorealisLog();
    return ok;
}

inline void startupStage(const char* stage) {
    switch_crashlog_stage(stage);
    log_msg("[startup] %s\n", stage);
}

inline bool isApplicationMode() {
    AppletType type = appletGetAppletType();
    return type == AppletType_Application ||
           type == AppletType_SystemApplication;
}

inline void showApplicationModeRequired() {
    consoleInit(nullptr);
    std::printf("\npipensx requires application mode.\n\n");
    std::printf("Close hbmenu, then hold R while launching a game.\n");
    std::printf("Keep holding R until hbmenu opens, then start pipensx.\n\n");
    std::printf("Album applet mode does not provide enough memory and\n");
    std::printf("network sessions for the GUI torrent client.\n\n");
    std::printf("Press + to exit.\n");
    consoleUpdate(nullptr);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
    }
    consoleExit(nullptr);
}

inline std::string formatBytes(uint64_t bytes) {
    char buffer[32];
    fmt_bytes(buffer, sizeof(buffer), bytes);
    return buffer;
}

inline std::string formatSpeed(uint64_t bytes) {
    char buffer[32];
    fmt_speed(buffer, sizeof(buffer), bytes);
    return buffer;
}

inline uint64_t emaUpdate(uint64_t previous, uint64_t sample) {
    if (sample >= previous)
        return previous + (sample - previous) * 3 / 10;
    return previous - (previous - sample) * 3 / 10;
}

inline float progressOf(const DownloadTask& task) {
    if (!task.totalBytes)
        return 0.0f;
    return std::min(1.0f, static_cast<float>(task.completedBytes) /
                              static_cast<float>(task.totalBytes));
}

inline float installProgressOf(const DownloadTask& task) {
    if (!task.installTotalBytes)
        return 0.0f;
    return std::min(1.0f, static_cast<float>(task.installedBytes) /
                              static_cast<float>(task.installTotalBytes));
}

inline int percentOf(float progress) {
    return static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * 100.0f);
}

inline std::string formatEta(uint64_t remainingBytes, uint64_t speedBytesPerSecond) {
    if (!remainingBytes || !speedBytesPerSecond)
        return {};

    uint64_t seconds = remainingBytes / speedBytesPerSecond;
    if (remainingBytes % speedBytesPerSecond)
        ++seconds;
    if (seconds < 60)
        return std::to_string(seconds) + "s";

    uint64_t minutes = seconds / 60;
    if (minutes < 60)
        return std::to_string(minutes) + "m " +
               std::to_string(seconds % 60) + "s";

    uint64_t hours = minutes / 60;
    if (hours < 24)
        return std::to_string(hours) + "h " +
               std::to_string(minutes % 60) + "m";

    return std::to_string(hours / 24) + "d " +
           std::to_string(hours % 24) + "h";
}

inline NVGcolor statusColor(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Error:
            return nvgRGB(255, 69, 84);
        case DownloadStatus::Completed:
        case DownloadStatus::Installed:
            return nvgRGB(96, 220, 130);
        case DownloadStatus::Paused:
        case DownloadStatus::Removing:
            return nvgRGB(170, 170, 180);
        case DownloadStatus::Queued:
        case DownloadStatus::Checking:
        case DownloadStatus::Downloading:
        case DownloadStatus::Verifying:
        case DownloadStatus::Installing:
        case DownloadStatus::Committing:
            return nvgRGB(0, 195, 227);
    }
    return nvgRGB(0, 195, 227);
}

inline std::string taskStatusText(const DownloadTask& task) {
    auto withPercent = [](const std::string& label, int percent) {
        return label + " " + std::to_string(percent) + "%";
    };

    switch (task.status) {
        case DownloadStatus::Checking:
        case DownloadStatus::Downloading:
        case DownloadStatus::Verifying:
            return withPercent(pipensx::statusName(task.status),
                               percentOf(progressOf(task)));
        case DownloadStatus::Installing:
        case DownloadStatus::Committing:
            return withPercent(pipensx::statusName(task.status),
                               percentOf(installProgressOf(task)));
        case DownloadStatus::Paused: {
            int pct = percentOf(progressOf(task));
            return pct > 0 ? withPercent("Paused", pct) : "Paused";
        }
        case DownloadStatus::Error: {
            int pct = percentOf(progressOf(task));
            return pct > 0 ? withPercent("Error", pct) : "Error";
        }
        case DownloadStatus::Completed:
            return "Downloaded";
        case DownloadStatus::Installed:
            return "Installed";
        case DownloadStatus::Queued:
        case DownloadStatus::Removing:
            return pipensx::statusName(task.status);
    }
    return pipensx::statusName(task.status);
}
inline std::string placeholderLetter(const std::string& title) {
    size_t offset = 0;
    while (offset < title.size() &&
           std::isspace(static_cast<unsigned char>(title[offset])))
        ++offset;
    if (offset == title.size())
        return "?";
    unsigned char lead = static_cast<unsigned char>(title[offset]);
    size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    length = std::min(length, title.size() - offset);
    std::string letter = title.substr(offset, length);
    if (length == 1)
        letter[0] = static_cast<char>(std::toupper(lead));
    return letter;
}
}  // namespace pipensx::ui
