#include "realdebrid_provider.hpp"

#include <cctype>
#include <string>

namespace pipensx {

bool RealdebridProvider::createFromMagnet(const std::string& magnet,
                                          std::string& id,
                                          std::string& error) {
    return client_.createFromMagnet(magnet, id, error);
}

bool RealdebridProvider::createFromFile(const std::string& torrentPath,
                                        std::string& id,
                                        std::string& error) {
    return client_.createFromFile(torrentPath, id, error);
}

bool RealdebridProvider::fetchInfo(const std::string& id, DebridInfo& out,
                                   std::string& error) {
    RdTorrentInfo info;
    if (!client_.fetchInfo(id, info, error))
        return false;
    out = DebridInfo{};
    out.name = info.filename;
    out.bytes = info.bytes;
    out.progress = info.progress;
    out.rawState = info.status;

    auto failedStatus = [](const std::string& status) {
        std::string lower;
        lower.reserve(status.size());
        for (char c : status)
            lower.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return lower == "error" || lower == "magnet_error" ||
               lower == "virus" || lower == "dead";
    };

    // Hosted file links only appear once RD has finished its side. Prefer
    // "downloaded", but also Ready if links are already populated — some
    // responses briefly expose links while status lags on uploading/
    // compressing, and waiting forever on status alone leaves the UI at 99%.
    if (!info.files.empty() &&
        (info.status == "downloaded" || !info.links.empty()))
        out.phase = DebridInfo::Phase::Ready;
    else if (failedStatus(info.status))
        out.phase = DebridInfo::Phase::Failed;
    else if (info.status == "waiting_files_selection")
        out.phase = DebridInfo::Phase::AwaitingSelection;
    else if (!info.status.empty() && info.status != "queued" &&
             info.status != "magnet_conversion")
        out.phase = DebridInfo::Phase::Downloading;
    else
        out.phase = DebridInfo::Phase::Creating;

    for (const auto& f : info.files) {
        DebridFile df;
        df.id = f.id;
        df.path = f.path;
        df.bytes = f.bytes;
        out.files.push_back(std::move(df));
    }
    out.links = info.links;
    return true;
}

bool RealdebridProvider::selectFiles(const std::string& id,
                                     const std::vector<std::string>& fileIds,
                                     std::string& error) {
    return client_.selectFiles(id, fileIds, error);
}

bool RealdebridProvider::resolveDownloadUrl(const std::string& /*id*/,
                                            const DebridInfo& info,
                                            size_t kthSelected,
                                            const DebridFile& /*file*/,
                                            std::string& url,
                                            std::string& error) {
    if (kthSelected >= info.links.size()) {
        error = "No download link for the selected file.";
        return false;
    }
    return client_.unrestrictLink(info.links[kthSelected], url, error);
}

bool RealdebridProvider::remove(const std::string& id, std::string& error) {
    return client_.remove(id, error);
}

} // namespace pipensx
