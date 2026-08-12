// Tests for InstallJournal bencode serialize/load (IMPROVEMENT_PLAN F-B).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "install/install_journal.hpp"

using pipensx::install::InstallJournal;
using pipensx::install::PackageStreamState;

namespace {

InstallJournal makeSample() {
    InstallJournal journal;
    journal.packageId = "magnet:?xt=urn:btih:00112233445566778899";
    journal.packageSize = 123456789;
    journal.compressed = true;
    journal.backendState = std::string("opaque\0blob\xff", 12);

    PackageStreamState& s = journal.state;
    s.consumed = 987654;
    s.headerSize = 208;
    s.dataPosition = 987000;
    s.currentInputRemaining = 4096;
    s.entryIndex = 1;
    s.fileOpen = true;
    s.skipped = false;
    s.decoderPresent = true;
    s.decoderHeaderReady = true;
    s.blockMode = true;
    s.outputSize = 5000000;
    s.outputPosition = 2500000;
    s.blockSize = 65536;
    s.nextBlock = 38;
    s.entries.push_back({0, 700000, "00112233445566778899aabbccddeeff.ncz"});
    s.entries.push_back({700000, 300, "meta.cnmt.nca"});
    PackageStreamState::Section section;
    section.offset = 0x4000;
    section.size = 4980000;
    section.cryptoType = 3;
    for (size_t i = 0; i < 16; ++i) {
        section.key[i] = static_cast<uint8_t>(i * 13 + 7);
        section.counter[i] = static_cast<uint8_t>(i * 5 + 3);
    }
    s.sections.push_back(section);
    section.offset = 0x4000 + 4980000;
    section.size = 20000;
    section.cryptoType = 1;
    section.key.fill(0);
    section.counter.fill(0);
    s.sections.push_back(section);
    s.blockSizes = {65536, 65210, 64000, 12345};
    return journal;
}

void expectEqual(const InstallJournal& a, const InstallJournal& b) {
    assert(a.packageId == b.packageId);
    assert(a.packageSize == b.packageSize);
    assert(a.compressed == b.compressed);
    assert(a.backendState == b.backendState);
    const PackageStreamState& x = a.state;
    const PackageStreamState& y = b.state;
    assert(x.consumed == y.consumed);
    assert(x.headerSize == y.headerSize);
    assert(x.dataPosition == y.dataPosition);
    assert(x.currentInputRemaining == y.currentInputRemaining);
    assert(x.entryIndex == y.entryIndex);
    assert(x.fileOpen == y.fileOpen);
    assert(x.skipped == y.skipped);
    assert(x.decoderPresent == y.decoderPresent);
    assert(x.decoderHeaderReady == y.decoderHeaderReady);
    assert(x.blockMode == y.blockMode);
    assert(x.outputSize == y.outputSize);
    assert(x.outputPosition == y.outputPosition);
    assert(x.blockSize == y.blockSize);
    assert(x.nextBlock == y.nextBlock);
    assert(x.entries.size() == y.entries.size());
    for (size_t i = 0; i < x.entries.size(); ++i) {
        assert(x.entries[i].offset == y.entries[i].offset);
        assert(x.entries[i].size == y.entries[i].size);
        assert(x.entries[i].name == y.entries[i].name);
    }
    assert(x.sections.size() == y.sections.size());
    for (size_t i = 0; i < x.sections.size(); ++i) {
        assert(x.sections[i].offset == y.sections[i].offset);
        assert(x.sections[i].size == y.sections[i].size);
        assert(x.sections[i].cryptoType == y.sections[i].cryptoType);
        assert(x.sections[i].key == y.sections[i].key);
        assert(x.sections[i].counter == y.sections[i].counter);
    }
    assert(x.blockSizes == y.blockSizes);
}

void testRoundtrip() {
    InstallJournal journal = makeSample();
    std::string blob = journal.serialize();
    InstallJournal loaded;
    assert(loaded.load(blob.data(), blob.size()));
    expectEqual(journal, loaded);
}

void testRoundtripDefaults() {
    InstallJournal journal;  // all defaults, empty vectors
    std::string blob = journal.serialize();
    InstallJournal loaded;
    loaded.packageId = "stale";  // must be overwritten
    assert(loaded.load(blob.data(), blob.size()));
    expectEqual(journal, loaded);
}

void testTruncationNeverLoads() {
    std::string blob = makeSample().serialize();
    for (size_t size = 0; size < blob.size(); ++size) {
        InstallJournal loaded;
        assert(!loaded.load(blob.data(), size));
    }
}

void testRejectsMalformed() {
    std::string blob = makeSample().serialize();
    {
        // Trailing garbage.
        std::string bad = blob + "x";
        InstallJournal loaded;
        assert(!loaded.load(bad.data(), bad.size()));
    }
    {
        // Wrong version.
        std::string bad = blob;
        size_t pos = bad.rfind("1:vi1e");
        assert(pos != std::string::npos);
        bad[pos + 4] = '2';
        InstallJournal loaded;
        assert(!loaded.load(bad.data(), bad.size()));
    }
    {
        // Boolean out of range.
        std::string bad = blob;
        size_t pos = bad.find("10:compressedi1e");
        assert(pos != std::string::npos);
        bad[pos + 14] = '7';
        InstallJournal loaded;
        assert(!loaded.load(bad.data(), bad.size()));
    }
    {
        // Missing state dict entirely.
        std::string bad = "d7:backend0:10:compressedi0e7:package2:idi"
                          "4:sizei0e1:vi1ee";
        InstallJournal loaded;
        assert(!loaded.load(bad.data(), bad.size()));
    }
    {
        // Section key of wrong length (15 bytes).
        std::string bad = blob;
        size_t pos = bad.find("3:key16:");
        assert(pos != std::string::npos);
        bad.replace(pos, 8, "3:key15:");
        bad.erase(pos + 8, 1);
        InstallJournal loaded;
        assert(!loaded.load(bad.data(), bad.size()));
    }
    {
        // Not bencode at all.
        InstallJournal loaded;
        const char garbage[] = "not a journal";
        assert(!loaded.load(garbage, sizeof garbage - 1));
        assert(!loaded.load(nullptr, 0));
    }
    {
        // Failed load leaves the previous contents untouched.
        InstallJournal loaded = makeSample();
        assert(!loaded.load(blob.data(), blob.size() - 1));
        expectEqual(loaded, makeSample());
    }
}

void testFileHelpers() {
    const std::string path = "/tmp/pipensx_test_install_journal.bin";
    std::remove(path.c_str());
    InstallJournal journal = makeSample();
    assert(pipensx::install::saveInstallJournal(path, journal));
    InstallJournal loaded;
    assert(pipensx::install::loadInstallJournal(path, loaded));
    expectEqual(journal, loaded);
    // Overwrite with different contents.
    journal.state.consumed += 1000;
    journal.packageId = "other";
    assert(pipensx::install::saveInstallJournal(path, journal));
    assert(pipensx::install::loadInstallJournal(path, loaded));
    expectEqual(journal, loaded);
    assert(pipensx::install::removeInstallJournal(path));
    assert(!pipensx::install::loadInstallJournal(path, loaded));
    assert(pipensx::install::removeInstallJournal(path));  // idempotent
}

} // namespace

int main() {
    testRoundtrip();
    testRoundtripDefaults();
    testTruncationNeverLoads();
    testRejectsMalformed();
    testFileHelpers();
    std::cout << "install journal tests passed\n";
    return 0;
}
