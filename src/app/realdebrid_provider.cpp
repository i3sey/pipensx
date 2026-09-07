#include "realdebrid_provider.hpp"
#include "nx_file_types.hpp"

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

namespace {

std::string rdBaseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    while (!name.empty() && name.front() == '/')
        name.erase(name.begin());
    return name;
}

bool sameName(const std::string& a, const std::string& b) {
    const std::string left = rdBaseName(a);
    const std::string right = rdBaseName(b);
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i])))
            return false;
    }
    return true;
}

bool mimeLooksLikeArchive(const std::string& mime) {
    std::string lower;
    lower.resize(mime.size());
    for (size_t i = 0; i < mime.size(); ++i)
        lower[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(mime[i])));
    return lower.find("zip") != std::string::npos ||
           lower.find("rar") != std::string::npos ||
           lower.find("7z") != std::string::npos ||
           lower.find("x-7z") != std::string::npos;
}

} // namespace

bool RealdebridProvider::resolveDownloadUrl(const std::string& /*id*/,
                                            const DebridInfo& info,
                                            size_t kthSelected,
                                            const DebridFile& file,
                                            std::string& url,
                                            std::string& error) {
    if (kthSelected >= info.links.size()) {
        error = "No download link for the selected file.";
        return false;
    }
    RdUnrestrict got;
    if (!client_.unrestrictLink(info.links[kthSelected], got, error))
        return false;
    if (isCompressedArchiveName(got.filename) ||
        mimeLooksLikeArchive(got.mimeType)) {
        error = "Real-Debrid returned an archive ('" +
                (got.filename.empty() ? got.mimeType : got.filename) +
                "'), not the NSP. Delete the torrent on Real-Debrid and retry.";
        return false;
    }
    if (got.filesize > 0 && file.bytes > 0 && got.filesize != file.bytes) {
        error = "Real-Debrid file size (" + std::to_string(got.filesize) +
                ") does not match '" + rdBaseName(file.path) + "' (" +
                std::to_string(file.bytes) +
                " bytes). Delete the torrent on Real-Debrid and retry.";
        return false;
    }
    if (!got.filename.empty() && isPackageName(file.path) &&
        !isPackageName(got.filename) && !sameName(got.filename, file.path)) {
        error = "Real-Debrid returned '" + rdBaseName(got.filename) +
                "' instead of '" + rdBaseName(file.path) +
                "'. Delete the torrent on Real-Debrid and retry.";
        return false;
    }
    url = std::move(got.url);
    return true;
}

bool RealdebridProvider::remove(const std::string& id, std::string& error) {
    return client_.remove(id, error);
}

} // namespace pipensx
