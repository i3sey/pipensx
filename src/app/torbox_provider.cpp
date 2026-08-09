#include "torbox_provider.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace pipensx {

bool TorboxProvider::createFromMagnet(const std::string& magnet,
                                      std::string& id, std::string& error) {
    uint64_t u64 = 0;
    if (!client_.createFromMagnet(magnet, u64, error))
        return false;
    id = std::to_string(u64);
    return true;
}

bool TorboxProvider::createFromFile(const std::string& torrentPath,
                                    std::string& id, std::string& error) {
    uint64_t u64 = 0;
    if (!client_.createFromFile(torrentPath, u64, error))
        return false;
    id = std::to_string(u64);
    return true;
}

bool TorboxProvider::fetchInfo(const std::string& id, DebridInfo& out,
                               std::string& error) {
    uint64_t u64 = std::strtoull(id.c_str(), nullptr, 10);
    TorboxTorrentInfo info;
    if (!client_.fetchInfo(u64, info, error))
        return false;
    out = DebridInfo{};
    out.name = info.name;
    out.bytes = info.size;
    out.progress = info.progress;
    out.rawState = info.state;
    // TorBox download_state values that mean the transfer is dead on their
    // side — surface Failed so pollUntilReady stops instead of spinning.
    auto failedState = [](std::string state) {
        for (char& c : state)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return state.find("failed") != std::string::npos ||
               state.find("error") != std::string::npos ||
               state.find("expired") != std::string::npos ||
               state == "deleted";
    };
    if (info.ready && !info.files.empty())
        out.phase = DebridInfo::Phase::Ready;
    else if (failedState(info.state))
        out.phase = DebridInfo::Phase::Failed;
    else
        out.phase = DebridInfo::Phase::Downloading;
    for (const auto& f : info.files) {
        DebridFile df;
        df.id = std::to_string(f.id);
        df.path = f.name;
        df.bytes = f.size;
        out.files.push_back(std::move(df));
    }
    return true;
}

bool TorboxProvider::resolveDownloadUrl(const std::string& id,
                                        const DebridInfo& /*info*/,
                                        size_t /*kthSelected*/,
                                        const DebridFile& file,
                                        std::string& url,
                                        std::string& error) {
    uint64_t torboxId = std::strtoull(id.c_str(), nullptr, 10);
    uint64_t fileId = std::strtoull(file.id.c_str(), nullptr, 10);
    return client_.requestDownloadLink(torboxId, fileId, url, error);
}

bool TorboxProvider::remove(const std::string& id, std::string& error) {
    uint64_t u64 = std::strtoull(id.c_str(), nullptr, 10);
    return client_.remove(u64, error);
}

} // namespace pipensx
