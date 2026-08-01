#pragma once

#include <curl/curl.h>

#include <string>

namespace pipensx {

// Refuse anything but HTTPS, on the request and on any redirect it follows.
// Debrid endpoints carry the account's API key, so a redirect to plain HTTP
// would leak it — and the download links are one redirect hop by design.
// The string form replaced the bitmask in 7.85; devkitPro can ship older.
inline void curlPinHttpsOnly(CURL* curl) {
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
}

// Same guarantee for a URL that may legitimately be plain HTTP — a TorrServer
// on the LAN cannot hold a certificate for a private address. The scheme the
// caller asked for is the only one allowed, so a redirect still cannot
// downgrade an HTTPS request.
inline void curlPinScheme(CURL* curl, const std::string& url) {
    const bool plain = url.compare(0, 7, "http://") == 0;
#if LIBCURL_VERSION_NUM >= 0x075500
    const char* protocols = plain ? "http" : "https";
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
#else
    const long protocols = plain ? CURLPROTO_HTTP : CURLPROTO_HTTPS;
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
}

} // namespace pipensx
