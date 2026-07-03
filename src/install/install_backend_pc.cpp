#include "install_backend.hpp"

#ifndef __SWITCH__

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx::install {
namespace {

bool makeDirectories(const std::string& path) {
    char buffer[1024];
    if (path.empty() || path.size() >= sizeof(buffer))
        return false;
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    for (char* p = buffer + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

std::string safeName(const std::string& value) {
    std::string result;
    for (unsigned char c : value)
        result.push_back(std::isalnum(c) || c == '.' || c == '-' ||
                         c == '_' ? static_cast<char>(c) : '_');
    return result.empty() ? "package" : result;
}

class PcInstallBackend final : public InstallBackend {
public:
    explicit PcInstallBackend(std::string root) : root_(std::move(root)) {}

    ~PcInstallBackend() override { rollbackPackage(); }

    bool beginPackage(const std::string& taskId,
                      const std::string& packageName) override {
        rollbackPackage();
        directory_ = root_ + "/install-sim/" + taskId + "-" +
                     safeName(packageName);
        if (!makeDirectories(directory_)) {
            error_ = "Unable to create PC installation output.";
            return false;
        }
        active_ = true;
        return true;
    }

    bool beginFile(const std::string& name, uint64_t size) override {
        if (!active_ || file_) {
            error_ = "Invalid PC installer file state.";
            return false;
        }
        expectedFile_ = size;
        writtenFile_ = 0;
        filePath_ = directory_ + "/" + safeName(name);
        file_ = std::fopen(filePath_.c_str(), "w+b");
        if (!file_) {
            error_ = "Unable to create PC installation file.";
            return false;
        }
        expected_ += size;
        return true;
    }

    bool setFileSize(uint64_t size) override {
        if (!file_ || expectedFile_ != 0) {
            error_ = "Invalid delayed file size.";
            return false;
        }
        expectedFile_ = size;
        expected_ += size;
        return true;
    }

    bool writeFile(const uint8_t* data, size_t size) override {
        if (!file_ || writtenFile_ + size > expectedFile_ ||
            std::fwrite(data, 1, size, file_) != size) {
            error_ = "Unable to write PC installation file.";
            return false;
        }
        writtenFile_ += size;
        installed_ += size;
        return true;
    }

    bool endFile() override {
        if (!file_ || writtenFile_ != expectedFile_) {
            error_ = "PC installation file size mismatch.";
            return false;
        }
        bool ok = std::fflush(file_) == 0 && std::fclose(file_) == 0;
        file_ = nullptr;
        if (!ok)
            error_ = "Unable to flush PC installation file.";
        return ok;
    }

    bool commitPackage(bool& alreadyInstalled) override {
        alreadyInstalled = false;
        if (!active_ || file_) {
            error_ = "PC package is incomplete.";
            return false;
        }
        active_ = false;
        directory_.clear();
        return true;
    }

    void rollbackPackage() override {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
        if (active_ && !filePath_.empty())
            unlink(filePath_.c_str());
        active_ = false;
        directory_.clear();
        filePath_.clear();
        expected_ = 0;
        installed_ = 0;
    }

    uint64_t installedBytes() const override { return installed_; }
    uint64_t expectedBytes() const override { return expected_; }
    const std::string& error() const override { return error_; }

private:
    std::string root_;
    std::string directory_;
    std::string filePath_;
    std::string error_;
    FILE* file_ = nullptr;
    uint64_t expectedFile_ = 0;
    uint64_t writtenFile_ = 0;
    uint64_t expected_ = 0;
    uint64_t installed_ = 0;
    bool active_ = false;
};

} // namespace

std::unique_ptr<InstallBackend> createInstallBackend(
    const std::string& workingRoot, InstallStorageTarget target) {
    // The PC backend writes NCAs to disk; the storage target is a Switch-only
    // (ncm) concern, so it is accepted for signature parity and ignored.
    (void)target;
    return std::make_unique<PcInstallBackend>(workingRoot);
}

} // namespace pipensx::install

#endif
