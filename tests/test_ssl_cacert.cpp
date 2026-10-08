#include "app/curl_https.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using pipensx::isSslCertificateErrorMessage;
using pipensx::urlHostForDisplay;

static std::string readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    assert(in && "resources/ssl/cacert.pem missing — run from repo root");
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

int main() {
    // Exercise real socket options. A rejected tuning request must still
    // let curl keep the socket; accepted sizes may be clamped by the OS.
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    pipensx::CurlSocketTuning tuning;
    assert(pipensx::curlEnlargeReceiveBuffer(&tuning, fd, CURLSOCKTYPE_IPCXN)
           == CURL_SOCKOPT_OK);
    assert(tuning.requestedBytes > 0 && tuning.actualBytes > 0);
    int actual = 0;
    socklen_t length = sizeof(actual);
    assert(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &length) == 0);
    assert(actual == tuning.actualBytes);
    close(fd);
    tuning = {};
    assert(pipensx::curlEnlargeReceiveBuffer(&tuning, CURL_SOCKET_BAD,
                                            CURLSOCKTYPE_IPCXN) == CURL_SOCKOPT_OK);
    assert(tuning.requestedBytes == 0 && tuning.actualBytes == 0);
    assert(tuning.lastError != 0);
    const std::string pem = readFile("resources/ssl/cacert.pem");
    assert(pem.find("BEGIN CERTIFICATE") != std::string::npos);
    int certs = 0;
    for (size_t i = 0; (i = pem.find("BEGIN CERTIFICATE", i)) != std::string::npos;
         i += 16)
        ++certs;
    // GTS Root R4 + GlobalSign + WE1 + cross-signed GTS + DigiCert Global Root G2.
    assert(certs >= 5);
    assert(pem.find("DigiCert Global Root G2") != std::string::npos);

    assert(isSslCertificateErrorMessage(
        "TorBox request failed: SSL peer certificate or SSH remote key was not "
        "OK"));
    assert(isSslCertificateErrorMessage(
        "SSL certificate problem: unable to get local issuer certificate"));
    assert(isSslCertificateErrorMessage("certificate verify failed"));
    assert(!isSslCertificateErrorMessage(
        "TorBox key rejected - relink in Settings."));
    assert(!isSslCertificateErrorMessage("Unable to reach TorrServer."));

    assert(urlHostForDisplay("http://user:secret@192.0.2.1:8090/play/x/1") ==
           "192.0.2.1:8090");
    assert(urlHostForDisplay("https://cdn.example.test/file") ==
           "cdn.example.test");
    assert(urlHostForDisplay("http://[2001:db8::1]:8090/echo") ==
           "[2001:db8::1]:8090");

    std::puts("test_ssl_cacert ok");
    return 0;
}
