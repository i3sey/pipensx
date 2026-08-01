#include "app/torbox_pairing_server.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using pipensx::HttpRequest;
using pipensx::HttpResponse;
using pipensx::TorboxPairingServer;

namespace {

HttpRequest get(const std::string& path) {
    HttpRequest request;
    request.method = "GET";
    request.path = path;
    return request;
}

HttpRequest post(const std::string& body) {
    HttpRequest request;
    request.method = "POST";
    request.path = "/key";
    request.body = body;
    return request;
}

void testParsePostKey() {
    std::string key;
    assert(TorboxPairingServer::parsePostKey("key=abc-123", key));
    assert(key == "abc-123");
    assert(TorboxPairingServer::parsePostKey("other=1&key=a%2Db+c", key));
    assert(key == "a-b c");
    // A field merely ending in "key" is not the key field.
    assert(!TorboxPairingServer::parsePostKey("apikey=nope", key));
    assert(!TorboxPairingServer::parsePostKey("nokey=1", key));
    assert(!TorboxPairingServer::parsePostKey("", key));
    // Surrounding whitespace comes from a phone's paste, not from the user.
    assert(TorboxPairingServer::parsePostKey("key=++pad++", key));
    assert(key == "pad");
}

void testHandleRequestFlow() {
    bool accepted = false;
    std::string key;
    auto validator = [](const std::string& candidate, std::string& error) {
        if (candidate == "good-key")
            return true;
        error = "That key was rejected by TorBox.";
        return false;
    };

    HttpResponse form =
        TorboxPairingServer::handleRequest(get("/"), validator, accepted, key);
    assert(form.status == 200);
    assert(form.body->find("name=\"key\"") != std::string::npos);
    assert(!accepted);

    HttpResponse bad = TorboxPairingServer::handleRequest(
        post("key=bad"), validator, accepted, key);
    assert(bad.status == 200);
    assert(bad.body->find("rejected") != std::string::npos);
    assert(!accepted);

    HttpResponse good = TorboxPairingServer::handleRequest(
        post("key=good-key"), validator, accepted, key);
    assert(good.status == 200);
    assert(accepted);
    assert(key == "good-key");

    accepted = false;
    HttpResponse missing = TorboxPairingServer::handleRequest(
        get("/nope"), validator, accepted, key);
    assert(missing.status == 404);
    assert(!accepted);

    HttpResponse malformed = TorboxPairingServer::handleRequest(
        post("garbage"), validator, accepted, key);
    assert(malformed.status == 200);
    assert(malformed.body->find("Invalid request format") != std::string::npos);
    assert(!accepted);
}

// The error message is rendered into the page, so a provider that echoes user
// input back must not be able to inject markup through it.
void testValidatorErrorIsEscaped() {
    bool accepted = false;
    std::string key;
    auto validator = [](const std::string&, std::string& error) {
        error = "<script>alert(1)</script>";
        return false;
    };
    HttpResponse rejected = TorboxPairingServer::handleRequest(
        post("key=x"), validator, accepted, key);
    assert(rejected.body->find("<script>") == std::string::npos);
    assert(rejected.body->find("&lt;script&gt;") != std::string::npos);
}

} // namespace

int main() {
    testParsePostKey();
    testHandleRequestFlow();
    testValidatorErrorIsEscaped();
    std::printf("test_torbox_pairing ok\n");
    return 0;
}
