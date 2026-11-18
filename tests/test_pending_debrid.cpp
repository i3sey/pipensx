#include "app/download_manager.hpp"
#include "app/pending_debrid.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace pipensx;

namespace {

std::string makeTempRoot() {
    char tmpl[] = "/tmp/pipensx-pending-debrid-XXXXXX";
    char* created = mkdtemp(tmpl);
    assert(created != nullptr);
    return std::string(created);
}

void testStoreCrudPersistenceAndTtl() {
    const std::string root = makeTempRoot();
    {
        PendingDebridStore store(root);
        assert(store.size() == 0);
        assert(store.list().empty());

        std::string error;
        const uint64_t initialGen = store.generation();

        PendingDebridEntry first;
        first.provider = DebridProviderKind::TorBox;
        first.infoHash = "AABBCCDDAABBCCDDAABBCCDDAABBCCDDAABBCCDD";
        first.debridId = "101";
        first.title = "Test Title";
        first.progress = 0.42;
        first.ready = false;
        first.createdAt = 1000;
        first.updatedAt = 1000;
        assert(store.upsert(first, error));
        assert(error.empty());
        assert(store.generation() == initialGen + 1);

        auto found = store.find(
            DebridProviderKind::TorBox,
            "aabbccddaabbccddaabbccddaabbccddaabbccdd");
        assert(found.has_value());
        assert(found->infoHash == "aabbccddaabbccddaabbccddaabbccddaabbccdd");
        assert(found->debridId == "101");
        assert(found->title == "Test Title");
        assert(found->progress > 0.41 && found->progress < 0.43);
        assert(!found->ready);
        assert(found->createdAt == 1000);

        // Upsert updates progress/id while preserving createdAt and keeping
        // empty title fallback.
        PendingDebridEntry update = first;
        update.debridId = "102";
        update.title.clear();
        update.progress = 1.5;  // clamped to 1.0
        update.ready = true;
        update.createdAt = 2000;
        update.updatedAt = 2500;
        assert(store.upsert(update, error));

        auto updated = store.find(
            DebridProviderKind::TorBox,
            "AABBCCDDAABBCCDDAABBCCDDAABBCCDDAABBCCDD");
        assert(updated.has_value());
        assert(updated->debridId == "102");
        assert(updated->title == "Test Title");
        assert(updated->progress == 1.0);
        assert(updated->ready);
        assert(updated->createdAt == 1000);
        assert(updated->updatedAt == 2500);

        // Separate provider key with the same hash is tracked independently.
        PendingDebridEntry rd;
        rd.provider = DebridProviderKind::RealDebrid;
        rd.infoHash = "aabbccddaabbccddaabbccddaabbccddaabbccdd";
        rd.debridId = "rd-99";
        rd.title = "RD Copy";
        rd.progress = 0.8;
        rd.createdAt = 3000;
        rd.updatedAt = 3000;
        assert(store.upsert(rd, error));
        assert(store.size() == 2);
    }

    // Reload from disk.
    {
        PendingDebridStore reloaded(root);
        assert(reloaded.size() == 2);
        auto tb = reloaded.find(
            DebridProviderKind::TorBox,
            "aabbccddaabbccddaabbccddaabbccddaabbccdd");
        assert(tb.has_value());
        assert(tb->debridId == "102");
        assert(tb->ready);

        // Prune items >= 7 days old relative to updatedAt.
        const int64_t now = 2500 + PendingDebridStore::kDefaultTtlSeconds;
        auto expired = reloaded.pruneExpired(now);
        assert(expired.size() == 1);
        assert(expired[0].debridId == "102");
        assert(reloaded.size() == 1);
        assert(!reloaded.find(
            DebridProviderKind::TorBox,
            "aabbccddaabbccddaabbccddaabbccddaabbccdd").has_value());
        assert(reloaded.find(
            DebridProviderKind::RealDebrid,
            "aabbccddaabbccddaabbccddaabbccddaabbccdd").has_value());

        std::string error;
        assert(reloaded.remove(
            DebridProviderKind::RealDebrid,
            "aabbccddaabbccddaabbccddaabbccddaabbccdd", error));
        assert(reloaded.size() == 0);
        // Removing missing key is idempotent.
        assert(reloaded.remove(
            DebridProviderKind::RealDebrid,
            "aabbccddaabbccddaabbccddaabbccddaabbccdd", error));

        PendingDebridEntry a;
        a.infoHash = "1111111111111111111111111111111111111111";
        a.debridId = "1";
        a.createdAt = 100;
        a.updatedAt = 100;
        PendingDebridEntry b = a;
        b.infoHash = "2222222222222222222222222222222222222222";
        b.debridId = "2";
        assert(reloaded.upsert(a, error));
        assert(reloaded.upsert(b, error));
        auto cleared = reloaded.clearAll(error);
        assert(cleared.size() == 2);
        assert(reloaded.size() == 0);
    }

    assert(std::system(("rm -rf " + root).c_str()) == 0);
}

void testNotFoundErrorHelpers() {
    assert(isDebridNotFoundError("TorBox torrent not found."));
    assert(isDebridNotFoundError("HTTP 404 while polling"));
    assert(isDebridNotFoundError("unknown_ressource"));
    assert(isDebridNotFoundError("MAGNET_INVALID_ID"));
    assert(isDebridNotFoundError("Transfer deleted on server"));
    assert(isDebridNotFoundError("Torrent expired"));
    assert(!isDebridNotFoundError("Timeout was reached."));
    assert(!isDebridNotFoundError(""));
}

void testDownloadManagerPrunesOnInitAndClearsOnImport() {
    const std::string root = makeTempRoot();
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    {
        PendingDebridStore pre(root);
        std::string error;
        PendingDebridEntry stale;
        stale.provider = DebridProviderKind::TorBox;
        stale.infoHash = "1111111111111111111111111111111111111111";
        stale.debridId = "old-1";
        stale.createdAt = 100;
        stale.updatedAt = 100;
        assert(pre.upsert(stale, error));

        PendingDebridEntry fresh;
        fresh.provider = DebridProviderKind::TorBox;
        fresh.infoHash = "2222222222222222222222222222222222222222";
        fresh.debridId = "fresh-2";
        fresh.createdAt = now;
        fresh.updatedAt = now;
        assert(pre.upsert(fresh, error));
    }

    {
        DownloadManager manager(root, false);
        assert(!manager.pendingDebrid().find(
            DebridProviderKind::TorBox,
            "1111111111111111111111111111111111111111").has_value());
        assert(manager.pendingDebrid().find(
            DebridProviderKind::TorBox,
            "2222222222222222222222222222222222222222").has_value());

        DebridImport imp;
        imp.infoHash = "2222222222222222222222222222222222222222";
        imp.name = "Game.nsp";
        imp.totalBytes = 1024;
        imp.provider = DebridProviderKind::TorBox;
        imp.debridId = "fresh-2";
        imp.mode = TransferMode::DownloadOnly;
        std::string taskId;
        std::string error;
        assert(manager.importDebrid(imp, taskId, error));
        assert(!manager.pendingDebrid().find(
            DebridProviderKind::TorBox,
            "2222222222222222222222222222222222222222").has_value());
    }

    assert(std::system(("rm -rf " + root).c_str()) == 0);
}

} // namespace

int main() {
    testStoreCrudPersistenceAndTtl();
    testNotFoundErrorHelpers();
    testDownloadManagerPrunesOnInitAndClearsOnImport();
    std::printf("test_pending_debrid ok\n");
    return 0;
}
