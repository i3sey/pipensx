#pragma once

#include "http_server.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace pipensx {

// One-page form on the LAN so a debrid API key can be pasted from a phone
// instead of typed on the console keyboard. Its own HttpServer on its own
// port: the web companion is a separate opt-in feature that may be off, and
// this listener has to die with the pairing screen either way.
//
// Trust model: while the pairing screen is open, any host that can reach the
// console on this port may POST a key/URL (no PIN). Bound to a short window
// and a one-shot accept; do not leave the screen idle on an untrusted LAN.
// What the phone form tells the user to paste. TorBox hands out an API key;
// a TorrServer is identified by its address, so the caller overrides it.
inline constexpr const char* kTorboxPairingHint =
    "Paste your TorBox API key. Find it at torbox.app > Settings > API.";

class TorboxPairingServer {
public:
    // validator returns true when the key is accepted; error is shown on the
    // phone form otherwise. Called on the server thread (may block on HTTP).
    using Validator = std::function<bool(const std::string& key,
                                         std::string& error)>;

    TorboxPairingServer(uint16_t port, Validator validator,
                        std::string hint = kTorboxPairingHint);
    ~TorboxPairingServer();
    bool start(std::string& error);   // binds 0.0.0.0:port, spawns thread
    void stop();                       // idempotent, joins thread
    bool keyAccepted() const;          // true once a key validated
    std::string acceptedKey() const;

    // Pure request handling (unit-tested). Sets keyAccepted/key when a POSTed
    // key passes the validator.
    static HttpResponse handleRequest(const HttpRequest& request,
                                      const Validator& validator,
                                      bool& keyAccepted, std::string& key,
                                      const std::string& hint =
                                          kTorboxPairingHint);
    // Reads the "key" field out of an application/x-www-form-urlencoded body.
    static bool parsePostKey(const std::string& body, std::string& key);

private:
    uint16_t port_;
    Validator validator_;
    std::string hint_;
    HttpServer server_;
    uint64_t startedMs_ = 0;
    std::atomic<bool> accepted_{false};
    mutable std::mutex mutex_;
    std::string key_;
};

constexpr uint16_t kTorboxPairingPort = 8424;

} // namespace pipensx
