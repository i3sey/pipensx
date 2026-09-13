#include "install/install_backend.hpp"
#include "install/install_journal.hpp"
#include "install/system_cleanup.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string rootPath() {
    return "/tmp/pipensx-system-cleanup-" +
           std::to_string(static_cast<long long>(getpid()));
}

void testDiscardSavedInstallRollsBackBeforeJournalRemoval() {
    const std::string root = rootPath();
    mkdir(root.c_str(), 0755);
    const std::string taskId = "cleanup-task";
    const std::string package = "partial.nsp";
    auto backend = pipensx::install::createInstallBackend(
        root, pipensx::install::InstallStorageTarget::SdCard);
    assert(backend->beginPackage(taskId, package));
    assert(backend->beginFile("partial.nca", 8));
    const uint8_t bytes[4] = {1, 2, 3, 4};
    assert(backend->writeFile(bytes, sizeof(bytes)));
    const std::string checkpoint = backend->checkpointPackage();
    assert(!checkpoint.empty());
    backend->suspendPackage();

    pipensx::install::InstallJournal journal;
    journal.packageId = package;
    journal.packageSize = 8;
    journal.backendState = checkpoint;
    const std::string journalPath =
        pipensx::install::installJournalPath(root, taskId);
    assert(pipensx::install::saveInstallJournal(journalPath, journal));
    const std::string partial =
        root + "/install-sim/cleanup-task-partial.nsp/partial.nca";
    assert(access(partial.c_str(), F_OK) == 0);

    std::string error;
    assert(pipensx::install::discardSavedInstall(root, taskId, error));
    assert(error.empty());
    assert(access(partial.c_str(), F_OK) != 0);
    assert(access(journalPath.c_str(), F_OK) != 0);

    rmdir((root + "/install-sim/cleanup-task-partial.nsp").c_str());
    rmdir((root + "/install-sim").c_str());
    rmdir(root.c_str());
}

void testDamagedJournalDoesNotBlockTaskRemoval() {
    const std::string root = rootPath() + "-damaged";
    mkdir(root.c_str(), 0755);
    const std::string path =
        pipensx::install::installJournalPath(root, "damaged-task");
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "damaged";
    std::string error;
    assert(pipensx::install::discardSavedInstall(
        root, "damaged-task", error));
    assert(error.empty());
    assert(access(path.c_str(), F_OK) != 0);
    rmdir(root.c_str());
}

} // namespace

int main() {
    testDiscardSavedInstallRollsBackBeforeJournalRemoval();
    testDamagedJournalDoesNotBlockTaskRemoval();
    std::puts("system cleanup tests passed");
    return 0;
}
