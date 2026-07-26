#include "snapshot_zstd.hpp"

#include <zstd.h>

namespace pipensx {

bool isZstdPath(const std::string& path) {
    static const char kSuffix[] = ".zst";
    const size_t n = sizeof(kSuffix) - 1;
    return path.size() >= n && path.compare(path.size() - n, n, kSuffix) == 0;
}

namespace {

bool decompressImpl(void* dst, size_t dstSize, const void* src, size_t srcSize,
                    std::string& error) {
    size_t written = ZSTD_decompress(dst, dstSize, src, srcSize);
    if (ZSTD_isError(written) || written != dstSize) {
        error = "Snapshot zstd decompression failed.";
        return false;
    }
    return true;
}

bool frameContentSize(const void* src, size_t srcSize, size_t maxBytes,
                      size_t& content, std::string& error) {
    unsigned long long declared = ZSTD_getFrameContentSize(src, srcSize);
    if (declared == ZSTD_CONTENTSIZE_ERROR ||
        declared == ZSTD_CONTENTSIZE_UNKNOWN || declared == 0) {
        error = "Snapshot has no valid zstd frame header.";
        return false;
    }
    if (declared > maxBytes) {
        error = "Snapshot decompresses larger than allowed.";
        return false;
    }
    content = static_cast<size_t>(declared);
    return true;
}

}  // namespace

bool decompressZstd(std::string& data, size_t maxBytes, std::string& error) {
    size_t content = 0;
    if (!frameContentSize(data.data(), data.size(), maxBytes, content, error))
        return false;
    std::string out;
    out.resize(content);
    if (!decompressImpl(out.data(), content, data.data(), data.size(), error))
        return false;
    data = std::move(out);
    return true;
}

bool decompressZstd(std::vector<uint8_t>& data, size_t maxBytes,
                    std::string& error) {
    size_t content = 0;
    if (!frameContentSize(data.data(), data.size(), maxBytes, content, error))
        return false;
    std::vector<uint8_t> out(content);
    if (!decompressImpl(out.data(), content, data.data(), data.size(), error))
        return false;
    data = std::move(out);
    return true;
}

}  // namespace pipensx
