#include "alldebrid_provider.hpp"
#include "nx_file_types.hpp"

#include <cctype>
#include <string>

namespace pipensx {
namespace {

std::string adBaseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    while (!name.empty() && name.front() == '/')
        name.erase(name.begin());
    return name;
}

bool sameName(const std::string& a, const std::string& b) {
    const std::string left = adBaseName(a);
    const std::string right = adBaseName(b);
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i])))
            return false;
    }
    return true;
}

std::string lowerCopy(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (char c : value)
        lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower;
}

bool looksLikeUrl(const std::string& value) {
    return value.compare(0, 8, "https://") == 0 ||
           value.compare(0, 7, "http://") == 0;
}

} // namespace

bool AlldebridProvider::createFromMagnet(const std::string& magnet,
                                         std::string& id,
                                         std::string& error) {
    return client_.createFromMagnet(magnet, id, error);
}

bool AlldebridProvider::createFromFile(const std::string& torrentPath,
                                       std::string& id,
                                       std::string& error) {
    return client_.createFromFile(torrentPath, id, error);
}

bool AlldebridProvider::fetchInfo(const std::string& id, DebridInfo& out,
                                  std::string& error) {
    AdTorrentInfo info;
    if (!client_.fetchInfo(id, info, error))
        return false;
    out = DebridInfo{};
    out.name = info.filename;
    out.bytes = info.bytes;
    out.progress = info.progress;
    out.rawState = info.status.empty()
        ? std::to_string(info.statusCode)
        : info.status;

    const std::string status = lowerCopy(info.status);
    const bool ready = info.statusCode == 4 || status == "ready";
    const bool failed = info.statusCode >= 5 || status == "error" ||
                        status == "expired" ||
                        status.find("error") != std::string::npos;
    if (ready && !info.files.empty())
        out.phase = DebridInfo::Phase::Ready;
    else if (failed)
        out.phase = DebridInfo::Phase::Failed;
    else if (info.statusCode == 0 || status == "in queue")
        out.phase = DebridInfo::Phase::Creating;
    else
        out.phase = DebridInfo::Phase::Downloading;

    for (const auto& f : info.files) {
        DebridFile df;
        df.id = f.id;
        df.path = f.path;
        df.bytes = f.bytes;
        out.files.push_back(std::move(df));
        if (!f.link.empty())
            out.links.push_back(f.link);
    }
    return true;
}

bool AlldebridProvider::resolveDownloadUrl(const std::string& /*id*/,
                                           const DebridInfo& info,
                                           size_t kthSelected,
                                           const DebridFile& file,
                                           std::string& url,
                                           std::string& error) {
    std::string hosted;
    if (looksLikeUrl(file.id))
        hosted = file.id;
    else if (kthSelected < info.links.size())
        hosted = info.links[kthSelected];
    if (hosted.empty()) {
        error = "No download link for the selected file.";
        return false;
    }
    AdUnrestrict got;
    if (!client_.unrestrictLink(hosted, got, error))
        return false;
    if (isCompressedArchiveName(got.filename)) {
        error = "AllDebrid returned an archive ('" + got.filename +
                "'), not the NSP. Delete the torrent on AllDebrid and retry.";
        return false;
    }
    if (got.filesize > 0 && file.bytes > 0 && got.filesize != file.bytes) {
        error = "AllDebrid file size (" + std::to_string(got.filesize) +
                ") does not match '" + adBaseName(file.path) + "' (" +
                std::to_string(file.bytes) +
                " bytes). Delete the torrent on AllDebrid and retry.";
        return false;
    }
    if (!got.filename.empty() && isPackageName(file.path) &&
        !isPackageName(got.filename) && !sameName(got.filename, file.path)) {
        error = "AllDebrid returned '" + adBaseName(got.filename) +
                "' instead of '" + adBaseName(file.path) +
                "'. Delete the torrent on AllDebrid and retry.";
        return false;
    }
    url = std::move(got.url);
    return true;
}

bool AlldebridProvider::remove(const std::string& id, std::string& error) {
    return client_.remove(id, error);
}

} // namespace pipensx
