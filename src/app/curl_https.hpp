#pragma once

#include <curl/curl.h>

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

} // namespace pipensx
