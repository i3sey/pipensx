#pragma once

#include <curl/curl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <cerrno>

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

struct CurlSocketTuning {
    int requestedBytes = 0;
    int actualBytes = 0;
    int lastError = 0;
};

// The full Switch profile backs a 1 MiB window with socket transfer memory.
// Applet mode and older services can reject or clamp it; keep those sockets
// usable and report what they actually accepted rather than assuming a gain.
inline int curlEnlargeReceiveBuffer(void* userdata, curl_socket_t socket,
                                    curlsocktype purpose) {
    if (purpose == CURLSOCKTYPE_IPCXN) {
        CurlSocketTuning tuning;
        for (const int size : {1024 * 1024, 512 * 1024, 256 * 1024, 128 * 1024}) {
            if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                           reinterpret_cast<const char*>(&size), sizeof(size)) == 0) {
                tuning.requestedBytes = size;
                break;
            }
            tuning.lastError = errno;
        }
        socklen_t length = sizeof(tuning.actualBytes);
        if (getsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<char*>(&tuning.actualBytes), &length) != 0)
            tuning.actualBytes = 0;
        if (userdata)
            *static_cast<CurlSocketTuning*>(userdata) = tuning;
        int nodelay = 1;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    }
    return CURL_SOCKOPT_OK;
}

inline void curlTuneDownloadSocket(CURL* curl, CurlSocketTuning* tuning = nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, curlEnlargeReceiveBuffer);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, tuning);
    // LAN TorrServer and WAN CDNs are both IPv4 in practice; skipping AAAA
    // waits on a hostname that has no v6 route.
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);
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

// Return only the host[:port] part suitable for logs and user-facing network
// errors. TorrServer permits http://user:password@host URLs; retaining the
// userinfo here leaked those credentials into bug-report logs.
inline std::string urlHostForDisplay(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end - start);
    const size_t userinfo = authority.rfind('@');
    if (userinfo != std::string::npos)
        authority.erase(0, userinfo + 1);
    return authority;
}

// Peer verify stays on. On Switch, also import the bundled roots in
// romfs:/ssl/cacert.pem (TorBox GTS + Real-Debrid / AllDebrid DigiCert; libnx curl uses
// sslContextImportServerPki — additive to the system store). On PC, leave the
// OpenSSL system store alone: CAINFO would replace it.
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
