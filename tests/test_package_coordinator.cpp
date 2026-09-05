#include "app/package_coordinator.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void append32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void append64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

std::vector<uint8_t> makePfs0(const std::string& name,
                              const std::vector<uint8_t>& file) {
    std::vector<uint8_t> out{'P', 'F', 'S', '0'};
    append32(out, 1);
    append32(out, static_cast<uint32_t>(name.size() + 1));
    append32(out, 0);
    append64(out, 0);
    append64(out, file.size());
    append32(out, 0);
    append32(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
    out.insert(out.end(), file.begin(), file.end());
    return out;
}

metainfo_t makeSingleFileMetainfo(const std::string& name, uint64_t size) {
    metainfo_t mi{};
    std::snprintf(mi.name, sizeof(mi.name), "%s", name.c_str());
    mi.piece_length = 4 * 1024 * 1024;
    mi.total_length = static_cast<int64_t>(size);
    mi.num_pieces = static_cast<uint32_t>(
        (size + static_cast<uint64_t>(mi.piece_length) - 1) /
        static_cast<uint64_t>(mi.piece_length));
    mi.piece_hashes = static_cast<uint8_t*>(std::calloc(mi.num_pieces, 20));
    assert(mi.piece_hashes);
    mi.num_files = 1;
    mi.files = static_cast<mi_file_t*>(std::calloc(1, sizeof(mi_file_t)));
    assert(mi.files);
    std::snprintf(mi.files[0].path, sizeof(mi.files[0].path), "%s",
                  name.c_str());
    mi.files[0].length = static_cast<int64_t>(size);
    mi.files[0].offset = 0;
    return mi;
}

bool feedRange(pipensx::PackageCoordinator& coordinator,
               const std::vector<uint8_t>& data, uint64_t offset,
               uint64_t end) {
    const auto& config = coordinator.configs()[0];
    const size_t chunkSize = 1024 * 1024;
    while (offset < end) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(chunkSize,
                                                          end - offset));
        if (!config.sink(config.user, 0, static_cast<int64_t>(offset),
                         data.data() + offset, n))
            return false;
        offset += n;
    }
    return true;
}

} // namespace

int main() {
    const std::string root = "/tmp/pipensx-package-coordinator-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    std::vector<uint8_t> nca(40 * 1024 * 1024, 0x5a);
    for (size_t i = 0; i < nca.size(); ++i)
        nca[i] = static_cast<uint8_t>(i * 17 + 3);
    std::vector<uint8_t> nsp = makePfs0(
        "00112233445566778899aabbccddeeff.nca", nca);

    metainfo_t mi = makeSingleFileMetainfo("game.nsp", nsp.size());
    const std::string taskId = "00112233445566778899aabbccddeeff00112233";
    const std::string journalPath = pipensx::installJournalPath(root, taskId);

    pipensx::install::InstallJournal saved;
    {
        pipensx::StreamBudgetArbiter arbiter;
        pipensx::PackageCoordinator coordinator(
            mi, taskId, root, true, {}, 0,
            pipensx::install::InstallStorageTarget::SdCard,
            arbiter, nullptr);
        assert(coordinator.error().empty());
        assert(coordinator.configs().size() == 1);
        assert(coordinator.configs()[0].mode == STORAGE_FILE_SINK);

        // Feed beyond the 32 MiB checkpoint interval, then simulate a torrent
        // network failure. The recoverable error must preserve the journal and
        // partial backend output instead of rolling back.
        assert(feedRange(coordinator, nsp, 0, 36ull * 1024 * 1024));
        bool journalLoaded = false;
        for (int spins = 0; spins < 3000; ++spins) {
            if (pipensx::install::loadInstallJournal(journalPath, saved)) {
                journalLoaded = true;
                break;
            }
            usleep(10000);
        }
        assert(journalLoaded);
        assert(saved.state.consumed > 0);
        coordinator.markRecoverableError("peer connection timed out");
    }

    assert(pipensx::install::loadInstallJournal(journalPath, saved));
    assert(saved.state.consumed > 0);
    uint64_t resumeOffset = saved.state.consumed;

    {
        pipensx::StreamBudgetArbiter arbiter;
        pipensx::PackageCoordinator coordinator(
            mi, taskId, root, true, {}, 0,
            pipensx::install::InstallStorageTarget::SdCard,
            arbiter, nullptr);
        assert(coordinator.error().empty());
        assert(coordinator.configs()[0].ready_bytes == resumeOffset);
        assert(feedRange(coordinator, nsp, resumeOffset, nsp.size()));
        assert(coordinator.finish());
    }

    // B4: a non-package download must fail naming the offending file and
    // its size, not with a bare "not a PFS0 NSP/NSZ".
    {
        const std::string badRoot = root + "-bad-package";
        std::filesystem::remove_all(badRoot);
        std::filesystem::create_directories(badRoot);
        const std::vector<uint8_t> garbage(64 * 1024, '<');
        metainfo_t badMi =
            makeSingleFileMetainfo("broken-pack.nsp", garbage.size());
        const std::string badTaskId =
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        pipensx::StreamBudgetArbiter badArbiter;
        pipensx::PackageCoordinator badCoordinator(
            badMi, badTaskId, badRoot, true, {}, 0,
            pipensx::install::InstallStorageTarget::SdCard,
            badArbiter, nullptr);
        assert(badCoordinator.error().empty());
        feedRange(badCoordinator, garbage, 0, garbage.size());
        bool errorSeen = false;
        for (int spins = 0; spins < 3000; ++spins) {
            if (!badCoordinator.error().empty()) {
                errorSeen = true;
                break;
            }
            usleep(10000);
        }
        assert(errorSeen);
        const std::string message = badCoordinator.error();
        assert(message.find("broken-pack.nsp") != std::string::npos);
        assert(message.find(std::to_string(garbage.size())) !=
               std::string::npos);
        assert(!badCoordinator.finish());
        metainfo_free(&badMi);
        std::filesystem::remove_all(badRoot);
    }

    struct stat st{};
    std::string committed = root + "/install-sim/" + taskId + "-game.nsp";
    assert(stat(committed.c_str(), &st) == 0);
    assert(!pipensx::install::loadInstallJournal(journalPath, saved));

    metainfo_free(&mi);
    std::filesystem::remove_all(root);
    puts("package coordinator tests passed");
    return 0;
}
