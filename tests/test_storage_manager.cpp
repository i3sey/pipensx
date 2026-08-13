#include "app/storage_manager.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string tempRoot(const char* tag) {
    return "/tmp/pipensx-storage-" + std::string(tag) + "-" +
           std::to_string(static_cast<long long>(getpid()));
}

void makeDir(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

void writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void testDirectorySizeFileAndMissing() {
    const std::string root = tempRoot("size");
    makeDir(root);
    writeFile(root + "/a.bin", "12345");
    uint64_t size = 0;
    assert(pipensx::directorySize(root + "/a.bin", size));
    assert(size == 5);
    uint64_t missing = 123;
    assert(!pipensx::directorySize(root + "/nope", missing));
    assert(missing == 123); // unchanged on failure
    unlink((root + "/a.bin").c_str());
    rmdir(root.c_str());
}

void testDirectorySizeRecursive() {
    const std::string root = tempRoot("rec");
    makeDir(root);
    makeDir(root + "/sub");
    writeFile(root + "/a.bin", "12345");
    writeFile(root + "/sub/b.bin", "1234567890");
    uint64_t size = 0;
    assert(pipensx::directorySize(root, size));
    assert(size == 15);
    unlink((root + "/a.bin").c_str());
    unlink((root + "/sub/b.bin").c_str());
    rmdir((root + "/sub").c_str());
    rmdir(root.c_str());
}

void testScanBreakdownBuckets() {
    const std::string root = tempRoot("scan");
    makeDir(root);
    makeDir(root + "/downloads");
    makeDir(root + "/torrents");
    makeDir(root + "/catalog");
    makeDir(root + "/catalog/images");
    makeDir(root + "/catalog/metadata");
    makeDir(root + "/installed-icons");
    writeFile(root + "/downloads/d.bin", "1234567890");
    writeFile(root + "/torrents/t.torrent", "12345");
    writeFile(root + "/catalog/images/i.jpg", "123456");
    writeFile(root + "/catalog/metadata/m.json", "1234");
    writeFile(root + "/installed-icons/x.jpg", "12");
    const pipensx::StorageBreakdown b = pipensx::scanStorageBreakdown(root);
    assert(b.downloadsBytes == 10);
    assert(b.torrentBytes == 5);
    assert(b.imageCacheBytes == 6);
    assert(b.metadataCacheBytes == 4);
    assert(b.iconsBytes == 2);
    assert(b.temporaryBytes == 0);
    // total/free come from statvfs of the temp dir; available should be true.
    assert(b.available);
    unlink((root + "/downloads/d.bin").c_str());
    unlink((root + "/torrents/t.torrent").c_str());
    unlink((root + "/catalog/images/i.jpg").c_str());
    unlink((root + "/catalog/metadata/m.json").c_str());
    unlink((root + "/installed-icons/x.jpg").c_str());
    rmdir((root + "/downloads").c_str());
    rmdir((root + "/torrents").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir((root + "/installed-icons").c_str());
    rmdir(root.c_str());
}

void testClearTemporaryFiles() {
    const std::string root = tempRoot("temp");
    makeDir(root);
    makeDir(root + "/install-temp");
    makeDir(root + "/install-temp/job");
    writeFile(root + "/install-temp/job/x", "1234567890");
    writeFile(root + "/_update_tmp_abc.torrent", "12345");
    writeFile(root + "/_catalog_tmp_def.torrent", "1234");
    writeFile(root + "/queue.bencode", "123");
    uint64_t recovered = 0;
    std::string error;
    assert(pipensx::clearTemporaryFiles(root, error, recovered));
    assert(recovered == 10 + 5 + 4);
    // queue.bencode survives; install-temp and tmp torrents are gone.
    uint64_t queueSize = 0;
    assert(pipensx::directorySize(root + "/queue.bencode", queueSize));
    assert(queueSize == 3);
    unlink((root + "/queue.bencode").c_str());
    rmdir(root.c_str());
}

void testClearOrphanTorrents() {
    const std::string root = tempRoot("orphan");
    makeDir(root);
    writeFile(root + "/aaaa.torrent", "111");
    writeFile(root + "/bbbb.torrent", "2222");
    writeFile(root + "/cccc.torrent", "33333");
    writeFile(root + "/notes.txt", "keep");
    std::vector<std::string> active = {"BBBB"};
    uint64_t recovered = 0;
    std::string error;
    // Estimate matches the bytes about to be recovered.
    assert(pipensx::orphanTorrentBytes(root, active) == 3 + 5);
    assert(pipensx::clearOrphanTorrents(root, active, error, recovered));
    assert(recovered == 3 + 5); // aaaa + cccc
    uint64_t bsize = 0;
    assert(pipensx::directorySize(root + "/bbbb.torrent", bsize));
    assert(bsize == 4);
    uint64_t nsize = 0;
    assert(pipensx::directorySize(root + "/notes.txt", nsize));
    assert(nsize == 4);
    unlink((root + "/bbbb.torrent").c_str());
    unlink((root + "/notes.txt").c_str());
    rmdir(root.c_str());
}

} // namespace

int main() {
    testDirectorySizeFileAndMissing();
    testDirectorySizeRecursive();
    testScanBreakdownBuckets();
    testClearTemporaryFiles();
    testClearOrphanTorrents();
    std::puts("storage manager tests passed");
    return 0;
}
