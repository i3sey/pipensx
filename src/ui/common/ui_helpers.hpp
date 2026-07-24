#pragma once

#include <switch.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include <borealis.hpp>

extern "C" {
#include "core/util.h"
}
#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "install/install_backend.hpp"
#include "platform/switch_crashlog.h"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

inline pipensx::install::InstallStorageTarget installTargetFor(InstallLocation value) {
    return value == InstallLocation::SystemMemory
               ? pipensx::install::InstallStorageTarget::Nand
               : pipensx::install::InstallStorageTarget::SdCard;
}

inline std::atomic<uint32_t> gCatalogTempSerial{0};
inline constexpr const char* TelemetryFlagPath =
    "sdmc:/switch/pipensx/throughput_telemetry.enabled";
inline constexpr const char* SettingsPath =
    "sdmc:/switch/pipensx/settings.json";
inline constexpr const char* LogPath =
    "sdmc:/switch/pipensx/pipensx.log";

// Point borealis at the log handle log_init() already owns. Opening the path
// a second time looks like it works on PC and writes nothing on the Switch,
// whose filesystem refuses to hand out a second handle on a file this process
// already has open - which silently dropped every borealis line on device.
inline void openBorealisLog() {
    if (FILE* file = log_file()) {
        brls::Logger::setLogOutput(file);
        brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
    }
}

inline bool clearApplicationLog() {
    // log_clear() truncates in place and keeps the handle, so borealis' output
    // target stays valid across the clear.
    return log_clear() != 0;
}

inline void startupStage(const char* stage) {
    switch_crashlog_stage(stage);
    log_msg("[startup] %s\n", stage);
}

// Write one structured "[diagnostic] schema=1 ... [system]" line capturing the
// build, firmware, storage and library state. Shared by Advanced's "Capture
// snapshot" action and the bug-report screen (which reads it straight back out
// of the log tail it encodes). diagnostic_snapshot() flushes to disk on its own.
inline void writeSystemSnapshot(DownloadManager* manager,
                                CatalogService* catalog,
                                GameMetadataService* metadata,
                                InstalledTitleService* installed,
                                const char* tag) {
    size_t active = 0;
    size_t errors = 0;
    for (const DownloadTask& task : manager->snapshot()) {
        if (task.status == DownloadStatus::Error)
            ++errors;
        else if (task.status != DownloadStatus::Completed &&
                 task.status != DownloadStatus::Installed &&
                 task.status != DownloadStatus::Paused)
            ++active;
    }
    uint64_t freeBytes = 0;
    struct statvfs storage {};
    if (statvfs("sdmc:/", &storage) == 0)
        freeBytes = static_cast<uint64_t>(storage.f_bavail) *
                    static_cast<uint64_t>(storage.f_frsize);
    uint32_t hos = hosversionGet();
    diagnostic_snapshot("system", tag,
        "version=%s hos=%u.%u.%u operation_mode=%d telemetry=%d "
        "catalog=%zu metadata=%zu installed=%zu active=%zu errors=%zu "
        "sd_free_bytes=%llu",
        PIPENSX_VERSION, HOSVER_MAJOR(hos), HOSVER_MINOR(hos),
        HOSVER_MICRO(hos), static_cast<int>(appletGetOperationMode()),
        telemetry_enabled(), catalog->entries().size(), metadata->size(),
        installed->titles().size(), active, errors,
        static_cast<unsigned long long>(freeBytes));
}

inline bool isApplicationMode() {
    AppletType type = appletGetAppletType();
    return type == AppletType_Application ||
           type == AppletType_SystemApplication;
}

inline void showApplicationModeRequired() {
    consoleInit(nullptr);
    std::printf("\npipensx requires application mode.\n\n");
    std::printf("Press HOME for exit.\n");
    std::printf("After that, hold R while launching a game.\n");
    std::printf("Keep holding R until hbmenu opens, then start pipensx.\n\n");
    std::printf("Album applet mode does not provide enough memory and\n");
    std::printf("network sessions for the GUI torrent client.\n\n");
    consoleUpdate(nullptr);

    while (appletMainLoop()) {
        svcSleepThread(100000000ULL);
    }
    consoleExit(nullptr);
}

// brls::RecyclerFrame culls cells against getVisibleFrame(), whose origin is
// the frame's offset inside its PARENT (ScrollingFrame::getVisibleFrame ->
// getLocalFrame), while cell positions are relative to the content top
// (RecyclerFrame::addCellAt accumulates y from 0). A recycler that is not its
// parent's first child therefore blanks the top N px of its own viewport, and
// because getNextCellFocus() can only focus a *rendered* cell, those rows drop
// out of gamepad navigation entirely. Being the sole child of a bare box pins
// that offset to 0. Keep the host padding/margin/border-free. Drop this the day
// borealis fixes getVisibleFrame().
inline brls::Box* recyclerHost(brls::RecyclerFrame* recycler) {
    auto* host = new brls::Box(brls::Axis::COLUMN);
    host->setGrow(1);
    host->addView(recycler);
    return host;
}

// The cells currently on screen — enough to repaint a row in place instead of
// paying a full reloadData(), which recycles every cell, snaps the scroll to 0
// and re-homes focus.
//
// RecyclerFrame keeps its cells in the content box it hands to
// setContentView(), but that is not the frame's only child: ScrollingFrame's
// constructor adds the scrolling indicator first. Which of the two ends up at
// index 0 falls out of Box::addView positioning by *yoga* child count while
// both views are detached — too subtle to depend on, so scan every child
// instead of assuming a slot.
template <typename Cell>
std::vector<Cell*> visibleCells(brls::RecyclerFrame* recycler) {
    std::vector<Cell*> cells;
    if (!recycler)
        return cells;
    for (brls::View* child : recycler->getChildren()) {
        auto* box = dynamic_cast<brls::Box*>(child);
        if (!box)
            continue;
        for (brls::View* view : box->getChildren()) {
            if (auto* cell = dynamic_cast<Cell*>(view))
                cells.push_back(cell);
        }
    }
    return cells;
}

inline std::string formatBytes(uint64_t bytes) {
    char buffer[32];
    fmt_bytes(buffer, sizeof(buffer), bytes);
    return buffer;
}

// fmt_bytes always prints two decimals ("118.24 GB"), which is too wide for the
// narrow sidebar column. Same units, no fraction.
inline std::string formatBytesShort(uint64_t bytes) {
    char buffer[32];
    if (bytes >= 1024ULL * 1024 * 1024)
        std::snprintf(buffer, sizeof(buffer), "%.0f GB",
                      bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        std::snprintf(buffer, sizeof(buffer), "%.0f MB",
                      bytes / (1024.0 * 1024));
    else if (bytes >= 1024ULL)
        std::snprintf(buffer, sizeof(buffer), "%.0f KB", bytes / 1024.0);
    else
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
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
        return tr("pipensx/downloads/eta_seconds", seconds);

    uint64_t minutes = seconds / 60;
    if (minutes < 60)
        return tr("pipensx/downloads/eta_minutes", minutes, seconds % 60);

    uint64_t hours = minutes / 60;
    if (hours < 24)
        return tr("pipensx/downloads/eta_hours", hours, minutes % 60);

    return tr("pipensx/downloads/eta_days", hours / 24, hours % 24);
}

inline NVGcolor statusColor(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Error:
            return theme::error();
        case DownloadStatus::Completed:
        case DownloadStatus::Installed:
            return theme::success();
        case DownloadStatus::Paused:
        case DownloadStatus::Removing:
            return theme::textSecondary();
        case DownloadStatus::Queued:
        case DownloadStatus::Checking:
        case DownloadStatus::Downloading:
        case DownloadStatus::Verifying:
        case DownloadStatus::Installing:
        case DownloadStatus::Committing:
            return theme::accent();
    }
    return theme::accent();
}

// pipensx::statusName stays the English persistence/log name (it is written
// into queue.bencode); the screen reads from the locale catalog instead.
inline std::string downloadStatusLabel(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Queued:
            return tr("pipensx/downloads/status_queued");
        case DownloadStatus::Checking:
            return tr("pipensx/downloads/status_checking");
        case DownloadStatus::Downloading:
            return tr("pipensx/downloads/status_downloading");
        case DownloadStatus::Paused:
            return tr("pipensx/downloads/status_paused");
        case DownloadStatus::Verifying:
            return tr("pipensx/downloads/status_verifying");
        case DownloadStatus::Completed:
            return tr("pipensx/downloads/status_completed");
        case DownloadStatus::Installing:
            return tr("pipensx/downloads/status_installing");
        case DownloadStatus::Committing:
            return tr("pipensx/downloads/status_committing");
        case DownloadStatus::Installed:
            return tr("pipensx/downloads/status_installed");
        case DownloadStatus::Error:
            return tr("pipensx/downloads/status_error");
        case DownloadStatus::Removing:
            return tr("pipensx/downloads/status_removing");
    }
    return tr("pipensx/common/unknown");
}

inline std::string taskStatusText(const DownloadTask& task) {
    auto withPercent = [](const std::string& label, int percent) {
        return label + " " + std::to_string(percent) + "%";
    };

    switch (task.status) {
        case DownloadStatus::Checking:
        case DownloadStatus::Downloading:
        case DownloadStatus::Verifying:
            return withPercent(downloadStatusLabel(task.status),
                               percentOf(progressOf(task)));
        case DownloadStatus::Installing:
        case DownloadStatus::Committing:
            return withPercent(downloadStatusLabel(task.status),
                               percentOf(installProgressOf(task)));
        case DownloadStatus::Paused:
        case DownloadStatus::Error: {
            const std::string label = downloadStatusLabel(task.status);
            int pct = percentOf(progressOf(task));
            return pct > 0 ? withPercent(label, pct) : label;
        }
        case DownloadStatus::Completed:
        case DownloadStatus::Installed:
        case DownloadStatus::Queued:
        case DownloadStatus::Removing:
            return downloadStatusLabel(task.status);
    }
    return downloadStatusLabel(task.status);
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
