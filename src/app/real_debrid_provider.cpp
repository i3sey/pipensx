#include "real_debrid_provider.hpp"

#include <sstream>

namespace pipensx {

bool RealDebridProvider::fetchInfo(const std::string& id, DebridInfo& out,
                                   std::string& error) {
    RdInfo rd;
    if (!client_.info(id, rd, error))
        return false;
    out = DebridInfo{};
    out.name = rd.filename;
    out.bytes = rd.bytes;
    out.progress = rd.progress;
    out.rawState = rd.status;

    const std::string& s = rd.status;
    if (s == "waiting_files_selection" || s == "magnet_conversion" ||
        s == "queued")
        out.phase = DebridInfo::Phase::AwaitingSelection;
    else if (s == "downloading" || s == "compressing" || s == "uploading")
        out.phase = DebridInfo::Phase::Downloading;
    else if (s == "downloaded")
        out.phase = DebridInfo::Phase::Ready;
    else if (s == "magnet_error" || s == "error" || s == "virus" ||
             s == "dead")
        out.phase = DebridInfo::Phase::Failed;
    else
        out.phase = DebridInfo::Phase::Creating;

    for (const auto& f : rd.files) {
        DebridFile df;
        df.id = std::to_string(f.id);
        df.path = f.path;
        df.bytes = f.bytes;
        out.files.push_back(std::move(df));
    }
    out.links = rd.links;
    return true;
}

bool RealDebridProvider::selectFiles(const std::string& id,
                                     const std::vector<std::string>& ids,
                                     std::string& error) {
    std::ostringstream csv;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0)
            csv << ',';
        csv << ids[i];
    }
    return client_.selectFiles(id, csv.str(), error);
}

bool RealDebridProvider::resolveDownloadUrl(const std::string& /*id*/,
                                            const DebridInfo& info,
                                            size_t kthSelected,
                                            const DebridFile& /*file*/,
                                            std::string& url,
                                            std::string& error) {
    if (kthSelected >= info.links.size()) {
        error = "RealDebrid link missing for selected file.";
        return false;
    }
    return client_.unrestrict(info.links[kthSelected], url, error);
}

} // namespace pipensx
