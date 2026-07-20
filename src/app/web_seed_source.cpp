#include "web_seed_source.hpp"

#include <cstdio>
#include <mutex>

#include <curl/curl.h>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {
namespace {

struct RangeBuffer {
    std::vector<uint8_t> data;
    size_t limit = 0;
    bool overflow = false;
};

size_t writeRange(char* ptr, size_t size, size_t nmemb, void* userdata) {
    RangeBuffer* buf = static_cast<RangeBuffer*>(userdata);
    size_t n = size * nmemb;
    if (buf->data.size() + n > buf->limit) {
        buf->overflow = true;
        return 0;
    }
    buf->data.insert(buf->data.end(), ptr, ptr + n);
    return n;
}

std::string resolveUrl(const std::string& base, const std::string& name) {
    // BEP-19: a URL ending in '/' is a base directory; append the file name.
    if (!base.empty() && base.back() == '/')
        return base + name;
    return base;
}

void ensureCurlGlobal() {
    // The app initialises curl at startup, but unit tests do not. Idempotent
    // (curl refcounts), so calling it once more here is harmless.
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace

WebSeedSource::WebSeedSource(const std::string& baseUrl, const std::string& name,
                             uint64_t pieceLength, uint64_t totalLength,
                             uint32_t numPieces, unsigned threads)
    : url_(resolveUrl(baseUrl, name)),
      pieceLength_(pieceLength),
      totalLength_(totalLength),
      numPieces_(numPieces) {
    ensureCurlGlobal();
    if (threads < 1)
        threads = 1;
    for (unsigned i = 0; i < threads; ++i)
        workers_.emplace_back([this] { workerMain(); });
    log_msg("[webseed] source %s pieces=%u plen=%llu threads=%u\n",
            url_.c_str(), numPieces_, (unsigned long long)pieceLength_, threads);
}

WebSeedSource::~WebSeedSource() {
    stop_.store(true);
    cv_.notify_all();
    for (std::thread& t : workers_)
        if (t.joinable())
            t.join();
}

uint64_t WebSeedSource::pieceLen(uint32_t piece) const {
    uint64_t offset = static_cast<uint64_t>(piece) * pieceLength_;
    if (offset >= totalLength_)
        return 0;
    uint64_t remaining = totalLength_ - offset;
    return remaining < pieceLength_ ? remaining : pieceLength_;
}

bool WebSeedSource::enqueue(uint32_t piece) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!assigned_.insert(piece).second)
        return false;
    pending_.push_back(piece);
    cv_.notify_one();
    return true;
}

size_t WebSeedSource::inFlight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assigned_.size();
}

bool WebSeedSource::popCompleted(Completed& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_.empty())
        return false;
    out = std::move(completed_.front());
    completed_.pop_front();
    assigned_.erase(out.piece);
    return true;
}

void WebSeedSource::workerMain() {
    for (;;) {
        uint32_t piece;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,
                     [this] { return stop_.load() || !pending_.empty(); });
            if (stop_.load())
                return;
            piece = pending_.front();
            pending_.pop_front();
        }
        Completed done;
        done.piece = piece;
        done.ok = fetchPiece(piece, done.data);
        std::lock_guard<std::mutex> lock(mutex_);
        completed_.push_back(std::move(done));
    }
}

bool WebSeedSource::fetchPiece(uint32_t piece, std::vector<uint8_t>& out) {
    uint64_t plen = pieceLen(piece);
    if (!plen)
        return false;
    uint64_t start = static_cast<uint64_t>(piece) * pieceLength_;
    uint64_t end = start + plen - 1;

    CURL* curl = curl_easy_init();
    if (!curl)
        return false;

    RangeBuffer buf;
    buf.limit = static_cast<size_t>(plen);
    buf.data.reserve(static_cast<size_t>(plen));

    char range[80];
    std::snprintf(range, sizeof(range), "%llu-%llu",
                  (unsigned long long)start, (unsigned long long)end);

    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeRange);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || buf.overflow)
        return false;
    // Require 206 Partial Content: a 200 means the server ignored the Range and
    // sent the whole file, which we can't map to a mid-file piece safely.
    if (status != 206)
        return false;
    if (buf.data.size() != plen)
        return false;
    out = std::move(buf.data);
    return true;
}

} // namespace pipensx
