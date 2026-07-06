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

// F-B journal blob helpers: little-endian, length-prefixed.
void putU64(std::string& out, uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

void putStr(std::string& out, const std::string& value) {
    putU64(out, value.size());
    out.append(value);
}

struct BlobReader {
    const char* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool raw(void* out, size_t count) {
        if (size - pos < count)
            return false;
        std::memcpy(out, data + pos, count);
        pos += count;
        return true;
    }
    bool u8(uint8_t& value) { return raw(&value, 1); }
    bool u64(uint64_t& value) {
        uint8_t bytes[8];
        if (!raw(bytes, sizeof(bytes)))
            return false;
        value = 0;
        for (int i = 0; i < 8; ++i)
            value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
        return true;
    }
    bool str(std::string& value) {
        uint64_t count = 0;
        if (!u64(count) || size - pos < count)
            return false;
        value.assign(data + pos, static_cast<size_t>(count));
        pos += static_cast<size_t>(count);
        return true;
    }
    bool done() const { return pos == size; }
};

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

    std::string checkpointPackage() override {
        if (!active_)
            return {};
        // Flush so the on-disk file matches the snapshot after a crash.
        if (file_ && std::fflush(file_) != 0)
            return {};
        std::string blob("PCB1");
        blob.push_back(file_ ? 1 : 0);
        putStr(blob, directory_);
        putStr(blob, filePath_);
        putU64(blob, expectedFile_);
        putU64(blob, writtenFile_);
        putU64(blob, expected_);
        putU64(blob, installed_);
        return blob;
    }

    void suspendPackage() override {
        if (!active_) {
            rollbackPackage();
            return;
        }
        if (file_) {
            std::fflush(file_);
            std::fclose(file_);
            file_ = nullptr;
        }
        // Keep the partial output on disk for resumePackage(); go idle.
        active_ = false;
        directory_.clear();
        filePath_.clear();
        expectedFile_ = 0;
        writtenFile_ = 0;
        expected_ = 0;
        installed_ = 0;
    }

    bool resumePackage(const std::string& taskId,
                       const std::string& packageName,
                       const std::string& state) override {
        (void)taskId;
        (void)packageName;
        rollbackPackage();
        BlobReader in { state.data(), state.size(), 0 };
        char magic[4] = {};
        uint8_t fileOpen = 0;
        std::string directory;
        std::string filePath;
        uint64_t expectedFile = 0;
        uint64_t writtenFile = 0;
        uint64_t expected = 0;
        uint64_t installed = 0;
        if (!in.raw(magic, sizeof(magic)) ||
            std::memcmp(magic, "PCB1", sizeof(magic)) != 0 ||
            !in.u8(fileOpen) || !in.str(directory) || !in.str(filePath) ||
            !in.u64(expectedFile) || !in.u64(writtenFile) ||
            !in.u64(expected) || !in.u64(installed) || !in.done() ||
            fileOpen > 1 || writtenFile > expectedFile) {
            error_ = "Install journal backend state is malformed.";
            return false;
        }
        struct stat st {};
        if (stat(directory.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            error_ = "PC installation directory is gone.";
            return false;
        }
        if (fileOpen) {
            FILE* file = std::fopen(filePath.c_str(), "r+b");
            if (!file) {
                error_ = "PC installation file is gone.";
                return false;
            }
            struct stat fst {};
            // Drop any bytes written after the checkpoint was taken; they
            // are replayed by the resumed stream.
            if (fstat(fileno(file), &fst) != 0 ||
                static_cast<uint64_t>(fst.st_size) < writtenFile ||
                ftruncate(fileno(file), static_cast<off_t>(writtenFile)) != 0 ||
                std::fseek(file, 0, SEEK_END) != 0) {
                std::fclose(file);
                error_ = "PC installation file does not match checkpoint.";
                return false;
            }
            file_ = file;
        }
        directory_ = std::move(directory);
        filePath_ = std::move(filePath);
        expectedFile_ = expectedFile;
        writtenFile_ = writtenFile;
        expected_ = expected;
        installed_ = installed;
        active_ = true;
        error_.clear();
        return true;
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
