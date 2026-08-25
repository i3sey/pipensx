#include "app/curl_https.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

using pipensx::isSslCertificateErrorMessage;

static std::string readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    assert(in && "resources/ssl/cacert.pem missing — run from repo root");
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

int main() {
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

    std::puts("test_ssl_cacert ok");
    return 0;
}
