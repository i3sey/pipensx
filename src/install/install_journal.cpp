#include "install_journal.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

extern "C" {
#include "../core/bencode.h"
}

namespace pipensx::install {

namespace {

// ---- writer (canonical bencode: dict keys emitted in sorted order) ----

void putInt(std::string& out, uint64_t value) {
    out += 'i';
    out += std::to_string(value);
    out += 'e';
}

void putStr(std::string& out, const char* data, size_t size) {
    out += std::to_string(size);
    out += ':';
    out.append(data, size);
}

void putStr(std::string& out, const std::string& value) {
    putStr(out, value.data(), value.size());
}

void putKey(std::string& out, const char* key) {
    putStr(out, key, std::strlen(key));
}

void putBool(std::string& out, bool value) {
    putInt(out, value ? 1 : 0);
}

// ---- reader (strict) ----

bool getNode(const be_node_t& dict, const char* key, be_type_t type,
             be_node_t& out) {
    if (!be_dict_get(dict.buf, dict.buf + dict.raw_len, key,
                     std::strlen(key), &out))
        return false;
    return out.type == type;
}

bool getU64(const be_node_t& dict, const char* key, uint64_t& out) {
    be_node_t node;
    if (!getNode(dict, key, BE_INT, node) || node.ival < 0)
        return false;
    out = static_cast<uint64_t>(node.ival);
    return true;
}

bool getU32(const be_node_t& dict, const char* key, uint32_t& out) {
    uint64_t value = 0;
    if (!getU64(dict, key, value) || value > UINT32_MAX)
        return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool getBool(const be_node_t& dict, const char* key, bool& out) {
    uint64_t value = 0;
    if (!getU64(dict, key, value) || value > 1)
        return false;
    out = value != 0;
    return true;
}

bool getStr(const be_node_t& dict, const char* key, std::string& out) {
    be_node_t node;
    if (!getNode(dict, key, BE_STR, node))
        return false;
    out.assign(node.sval, node.slen);
    return true;
}

bool getBytes16(const be_node_t& dict, const char* key,
                std::array<uint8_t, 16>& out) {
    be_node_t node;
    if (!getNode(dict, key, BE_STR, node) || node.slen != out.size())
        return false;
    std::memcpy(out.data(), node.sval, out.size());
    return true;
}

void serializeState(std::string& out, const PackageStreamState& state) {
    out += 'd';
    putKey(out, "blockMode");
    putBool(out, state.blockMode);
    putKey(out, "blockSize");
    putInt(out, state.blockSize);
    putKey(out, "blockSizes");
    out += 'l';
    for (uint32_t size : state.blockSizes)
        putInt(out, size);
    out += 'e';
    putKey(out, "consumed");
    putInt(out, state.consumed);
    putKey(out, "currentInputRemaining");
    putInt(out, state.currentInputRemaining);
    putKey(out, "dataPosition");
    putInt(out, state.dataPosition);
    putKey(out, "decoderHeaderReady");
    putBool(out, state.decoderHeaderReady);
    putKey(out, "decoderPresent");
    putBool(out, state.decoderPresent);
    putKey(out, "entries");
    out += 'l';
    for (const auto& entry : state.entries) {
        out += 'd';
        putKey(out, "name");
        putStr(out, entry.name);
        putKey(out, "offset");
        putInt(out, entry.offset);
        putKey(out, "size");
        putInt(out, entry.size);
        out += 'e';
    }
    out += 'e';
    putKey(out, "entryIndex");
    putInt(out, state.entryIndex);
    putKey(out, "fileOpen");
    putBool(out, state.fileOpen);
    putKey(out, "headerSize");
    putInt(out, state.headerSize);
    putKey(out, "nextBlock");
    putInt(out, state.nextBlock);
    putKey(out, "outputPosition");
    putInt(out, state.outputPosition);
    putKey(out, "outputSize");
    putInt(out, state.outputSize);
    putKey(out, "sections");
    out += 'l';
    for (const auto& section : state.sections) {
        out += 'd';
        putKey(out, "counter");
        putStr(out, reinterpret_cast<const char*>(section.counter.data()),
               section.counter.size());
        putKey(out, "cryptoType");
        putInt(out, section.cryptoType);
        putKey(out, "key");
        putStr(out, reinterpret_cast<const char*>(section.key.data()),
               section.key.size());
        putKey(out, "offset");
        putInt(out, section.offset);
        putKey(out, "size");
        putInt(out, section.size);
        out += 'e';
    }
    out += 'e';
    putKey(out, "skipped");
    putBool(out, state.skipped);
    out += 'e';
}

bool loadState(const be_node_t& dict, PackageStreamState& state) {
    if (!getBool(dict, "blockMode", state.blockMode) ||
        !getU64(dict, "blockSize", state.blockSize) ||
        !getU64(dict, "consumed", state.consumed) ||
        !getU64(dict, "currentInputRemaining", state.currentInputRemaining) ||
        !getU64(dict, "dataPosition", state.dataPosition) ||
        !getBool(dict, "decoderHeaderReady", state.decoderHeaderReady) ||
        !getBool(dict, "decoderPresent", state.decoderPresent) ||
        !getU32(dict, "entryIndex", state.entryIndex) ||
        !getBool(dict, "fileOpen", state.fileOpen) ||
        !getU64(dict, "headerSize", state.headerSize) ||
        !getU32(dict, "nextBlock", state.nextBlock) ||
        !getU64(dict, "outputPosition", state.outputPosition) ||
        !getU64(dict, "outputSize", state.outputSize) ||
        !getBool(dict, "skipped", state.skipped))
        return false;

    be_node_t list;
    if (!getNode(dict, "blockSizes", BE_LIST, list))
        return false;
    const char* p = list.buf + 1;
    const char* end = list.buf + list.raw_len;
    be_node_t item;
    while (be_list_next(&p, end, &item)) {
        if (item.type != BE_INT || item.ival < 0 || item.ival > UINT32_MAX)
            return false;
        state.blockSizes.push_back(static_cast<uint32_t>(item.ival));
    }

    if (!getNode(dict, "entries", BE_LIST, list))
        return false;
    p = list.buf + 1;
    end = list.buf + list.raw_len;
    while (be_list_next(&p, end, &item)) {
        if (item.type != BE_DICT)
            return false;
        PackageStreamState::Entry entry;
        if (!getStr(item, "name", entry.name) ||
            !getU64(item, "offset", entry.offset) ||
            !getU64(item, "size", entry.size))
            return false;
        state.entries.push_back(std::move(entry));
    }

    if (!getNode(dict, "sections", BE_LIST, list))
        return false;
    p = list.buf + 1;
    end = list.buf + list.raw_len;
    while (be_list_next(&p, end, &item)) {
        if (item.type != BE_DICT)
            return false;
        PackageStreamState::Section section;
        if (!getBytes16(item, "counter", section.counter) ||
            !getU64(item, "cryptoType", section.cryptoType) ||
            !getBytes16(item, "key", section.key) ||
            !getU64(item, "offset", section.offset) ||
            !getU64(item, "size", section.size))
            return false;
        state.sections.push_back(section);
    }
    return true;
}

} // namespace

std::string InstallJournal::serialize() const {
    std::string out;
    out += 'd';
    putKey(out, "backend");
    putStr(out, backendState);
    putKey(out, "compressed");
    putBool(out, compressed);
    putKey(out, "package");
    putStr(out, packageId);
    putKey(out, "size");
    putInt(out, packageSize);
    putKey(out, "state");
    serializeState(out, state);
    putKey(out, "v");
    putInt(out, static_cast<uint64_t>(kVersion));
    out += 'e';
    return out;
}

bool InstallJournal::load(const char* data, size_t size) {
    const char* p = data;
    be_node_t root;
    if (!data || !be_decode(&p, data + size, &root) || root.type != BE_DICT)
        return false;
    if (p != data + size)  // trailing garbage: not our file
        return false;

    InstallJournal parsed;
    uint64_t version = 0;
    if (!getU64(root, "v", version) ||
        version != static_cast<uint64_t>(kVersion))
        return false;
    if (!getStr(root, "backend", parsed.backendState) ||
        !getBool(root, "compressed", parsed.compressed) ||
        !getStr(root, "package", parsed.packageId) ||
        !getU64(root, "size", parsed.packageSize))
        return false;
    be_node_t stateDict;
    if (!getNode(root, "state", BE_DICT, stateDict) ||
        !loadState(stateDict, parsed.state))
        return false;

    *this = std::move(parsed);
    return true;
}

bool saveInstallJournal(const std::string& path,
                        const InstallJournal& journal) {
    const std::string blob = journal.serialize();
    const std::string tmp = path + ".tmp";
    std::FILE* file = std::fopen(tmp.c_str(), "wb");
    if (!file)
        return false;
    bool ok = std::fwrite(blob.data(), 1, blob.size(), file) == blob.size();
    ok = std::fflush(file) == 0 && ok;
#if !defined(_WIN32)
    if (ok)
        fsync(fileno(file));  // best effort; journal loss only costs a restart
#endif
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool loadInstallJournal(const std::string& path, InstallJournal& journal) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::vector<char> blob;
    char buffer[4096];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0)
        blob.insert(blob.end(), buffer, buffer + count);
    const bool readOk = std::ferror(file) == 0;
    std::fclose(file);
    return readOk && journal.load(blob.data(), blob.size());
}

bool removeInstallJournal(const std::string& path) {
    return std::remove(path.c_str()) == 0 || errno == ENOENT;
}

} // namespace pipensx::install
