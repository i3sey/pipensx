#include "realdebrid_provider.hpp"
#include "nx_file_types.hpp"

#include <cctype>
#include <string>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
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
           lower.find("7z") != std::string::npos;
}

std::string magnetInfoHash(const std::string& magnet) {
    const std::string marker = "urn:btih:";
    const size_t pos = magnet.find(marker);
    if (pos == std::string::npos)
        return {};
    const size_t start = pos + marker.size();
    size_t end = magnet.find('&', start);
    if (end == std::string::npos)
        end = magnet.size();
    std::string hash = magnet.substr(start, end - start);
    if (hash.size() != 40)
        return {};
    for (char& ch : hash) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isxdigit(c))
            return {};
        ch = static_cast<char>(std::tolower(c));
    }
    return hash;
}

} // namespace

bool RealdebridProvider::createFromMagnet(const std::string& magnet,
                                          std::string& id,
                                          std::string& error) {
    const std::string hash = magnetInfoHash(magnet);
    if (!hash.empty()) {
        std::string existing;
        std::string lookupError;
        if (client_.findTorrentByHash(hash, existing, lookupError) &&
            !existing.empty()) {
            id = std::move(existing);
            error.clear();
            log_msg("[realdebrid] reusing account torrent id=%s hash=%s\n",
                    id.c_str(), hash.c_str());
            return true;
        }
        if (!lookupError.empty())
            log_msg("[realdebrid] account torrent lookup failed; adding magnet: %s\n",
                    lookupError.c_str());
    }
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
    if (failedStatus(info.status))
        out.phase = DebridInfo::Phase::Failed;
    else if (!info.files.empty() &&
        (info.status == "downloaded" || (!info.links.empty() && info.progress >= 1.0)))
        out.phase = DebridInfo::Phase::Ready;
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
        df.serverSelected = f.selected;
        out.files.push_back(std::move(df));
    }
    out.links = info.links;
    size_t selectedCount = 0;
    for (const auto& file : out.files)
        if (file.serverSelected == 1) ++selectedCount;
    // HTTP 200 alone hides queued/selection/remote-copy stalls, especially
    // in preflight and grouped-file fallback, which have no transfer log yet.
    log_msg("[realdebrid] state id=%s status=%s progress=%.1f%% bytes=%llu files=%zu selected=%zu links=%zu phase=%d\n",
            id.c_str(), out.rawState.c_str(), out.progress * 100.0,
            (unsigned long long)out.bytes, out.files.size(), selectedCount,
            out.links.size(), static_cast<int>(out.phase));
    return true;
}

bool RealdebridProvider::selectFiles(const std::string& id,
                                     const std::vector<std::string>& fileIds,
                                     std::string& error) {
    selectionAlreadyFinalized_ = false;
    if (!client_.selectFiles(id, fileIds, error,
                             &selectionAlreadyFinalized_))
        return false;
    if (selectionAlreadyFinalized_)
        log_msg("[realdebrid] selection already finalized id=%s\n",
                id.c_str());
    return true;
}

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

bool RealdebridProvider::resolveGroupedDownloadUrl(const DebridInfo& info,
    size_t index, std::string& url, uint64_t& bytes, std::string& error) {
    if (index >= info.links.size()) {
        error = "Real-Debrid did not publish the grouped download link.";
        return false;
    }
    RdUnrestrict got;
    if (!client_.unrestrictLink(info.links[index], got, error)) return false;
    log_msg("[realdebrid] grouped payload filename='%s' bytes=%llu mime='%s'\n",
            got.filename.c_str(), (unsigned long long)got.filesize, got.mimeType.c_str());
    if (got.url.compare(0, 8, "https://") != 0 || !got.filesize) {
        error = "Real-Debrid grouped download requires HTTPS and a known file size.";
        return false;
    }
    // The ZIP reader checks the actual format, directory, member paths and
    // sizes. Filename/MIME alone must never authorize installing an archive.
    url = std::move(got.url);
    bytes = got.filesize;
    return true;
}

bool RealdebridProvider::createSelectionReplacement(const std::string& id,
                                                    std::string& newId,
                                                    std::string& error) {
    newId.clear();
    RdTorrentInfo original;
    if (!client_.fetchInfo(id, original, error))
        return false;
    if (original.hash.size() != 40 ||
        original.hash.find_first_not_of("0123456789abcdefABCDEF") !=
            std::string::npos) {
        error = "Real-Debrid did not return a valid torrent hash for "
                "selection recovery.";
        return false;
    }
    if (!client_.createFromMagnet("magnet:?xt=urn:btih:" + original.hash,
                                  newId, error))
        return false;
    if (newId == id) {
        newId.clear();
        error = "Real-Debrid reused the finalized torrent instead of creating "
                "a fresh selection.";
        return false;
    }
    return true;
}

} // namespace pipensx
