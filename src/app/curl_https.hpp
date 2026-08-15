#pragma once

#include <curl/curl.h>
#include <sys/socket.h>

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

// Borealis boots the Switch socket service with tiny default buffers;
// a larger receive window is what keeps long HTTPS transfers off the
// kilobytes-per-second floor. bsd:u silently clamps SO_RCVBUF at 256 KiB.
inline int curlEnlargeReceiveBuffer(void*, curl_socket_t socket,
                                    curlsocktype purpose) {
    if (purpose == CURLSOCKTYPE_IPCXN) {
        int size = 256 * 1024;
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size));
    }
    return CURL_SOCKOPT_OK;
}

inline void curlTuneDownloadSocket(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, curlEnlargeReceiveBuffer);
}

// HTTPS stays pinned so a debrid CDN cannot redirect onto plaintext. Plain
// HTTP (TorrServer on the LAN) is left on curl's default allow-list: Switch
// libcurl 7.69 has aborted the process when CURLOPT_PROTOCOLS was set to
// HTTP-only, which is how linking a TorrServer presented as std::terminate
// with no exception right after GET /echo.
inline void curlPinScheme(CURL* curl, const std::string& url) {
    if (url.compare(0, 7, "http://") == 0)
        return;
    curlPinHttpsOnly(curl);
}

// Peer verify stays on. On Switch, also import the bundled roots in
// romfs:/ssl/cacert.pem (libnx curl uses sslContextImportServerPki — additive
// to the system store). On PC, leave the OpenSSL system store alone: CAINFO
// would replace it.
inline void curlApplyTrustedSsl(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef __SWITCH__
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/ssl/cacert.pem");
#endif
}

// True for curl/OpenSSL peer-verify failure strings we surface with a
// "check console clock" hint after CAINFO still did not help.
inline bool isSslCertificateErrorMessage(const std::string& error) {
    return error.find("SSL peer certificate") != std::string::npos ||
           error.find("SSL certificate problem") != std::string::npos ||
           error.find("certificate verify failed") != std::string::npos;
}

} // namespace pipensx
