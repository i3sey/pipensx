#include "torbox_pairing_server.hpp"

#include <string>
#include <utility>

extern "C" {
#include "../core/util.h"
}

namespace pipensx {

namespace {

// Only half an hour, and only one key: the screen owning this server already
// tears it down when the user leaves, but a console parked on the pairing
// page overnight should not stay willing to have its account swapped.
constexpr uint64_t kPairingWindowMs = 30 * 60 * 1000;

std::string htmlEscape(const std::string& text) {
    std::string escaped;
    for (char c : text) {
        switch (c) {
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '&': escaped += "&amp;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

std::string formPage(const std::string& message) {
    return "<!doctype html><html><head><meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\">"
        "<title>Link TorBox</title></head>"
        "<body style=\"font-family:sans-serif;max-width:26em;margin:3em "
        "auto;padding:0 1em\"><h2>Link TorBox</h2>"
        "<p>Paste your TorBox API key. Find it at "
        "torbox.app &gt; Settings &gt; API.</p>" +
        (message.empty() ? std::string()
                         : "<p style=\"color:#b00\">" + htmlEscape(message) +
                           "</p>") +
        "<form method=\"post\" action=\"/key\">"
        "<input name=\"key\" style=\"width:100%;padding:.6em\" "
        "autocomplete=\"off\" autofocus>"
        "<button style=\"margin-top:1em;padding:.6em 2em\">Link</button>"
        "</form></body></html>";
}

std::string successPage() {
    return "<!doctype html><html><head><meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\">"
        "<title>Linked</title></head>"
        "<body style=\"font-family:sans-serif;max-width:26em;margin:3em "
        "auto;padding:0 1em\"><h2>Linked!</h2>"
        "<p>You can return to your Switch.</p>"
        "</body></html>";
}

HttpResponse page(int status, std::string body) {
    return HttpResponse::text(status, std::move(body),
                              "text/html; charset=utf-8");
}

// A phone posting one short form field: nothing here needs the companion's
// connection budget or its SSE machinery.
HttpServer::Options pairingOptions() {
    HttpServer::Options options;
    options.maxConnections = 2;
    options.maxSseClients = 0;
    options.bodyCap = 8 * 1024;
    return options;
}

} // namespace

TorboxPairingServer::TorboxPairingServer(uint16_t port, Validator validator)
    : port_(port), validator_(std::move(validator)),
      server_(pairingOptions()) {}

TorboxPairingServer::~TorboxPairingServer() {
    stop();
}

bool TorboxPairingServer::parsePostKey(const std::string& body,
                                       std::string& key) {
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos)
            amp = body.size();
        const std::string field = body.substr(pos, amp - pos);
        const size_t eq = field.find('=');
        if (eq != std::string::npos && field.compare(0, eq, "key") == 0) {
            key = httpUrlDecode(field.substr(eq + 1));
            const size_t start = key.find_first_not_of(" \t\r\n");
            const size_t end = key.find_last_not_of(" \t\r\n");
            key = start == std::string::npos
                      ? std::string()
                      : key.substr(start, end - start + 1);
            return true;
        }
        pos = amp + 1;
    }
    return false;
}

HttpResponse TorboxPairingServer::handleRequest(const HttpRequest& request,
                                                const Validator& validator,
                                                bool& keyAccepted,
                                                std::string& key) {
    keyAccepted = false;
    if (request.method == "GET" && request.path == "/")
        return page(200, formPage(""));
    if (request.method == "POST" && request.path == "/key") {
        std::string posted;
        if (!parsePostKey(request.body, posted))
            return page(200, formPage("Invalid request format."));
        std::string error;
        if (!validator(posted, error))
            return page(200, formPage(error));
        keyAccepted = true;
        key = posted;
        return page(200, successPage());
    }
    return page(404, "");
}

bool TorboxPairingServer::start(std::string& error) {
    accepted_ = false;
    startedMs_ = now_ms();
    if (server_.start(port_, [this](const HttpRequest& request) {
            if (accepted_ || now_ms() - startedMs_ > kPairingWindowMs)
                return page(410, "Pairing is closed. Reopen it on the "
                                 "console to link an account.");
            bool accepted = false;
            std::string key;
            HttpResponse response =
                handleRequest(request, validator_, accepted, key);
            if (accepted) {
                std::lock_guard<std::mutex> lock(mutex_);
                key_ = key;
                accepted_ = true;
            }
            return response;
        }, error))
        return true;
    error = "Unable to start the pairing server (port " +
            std::to_string(port_) + " busy?).";
    return false;
}

void TorboxPairingServer::stop() {
    server_.stop();
}

bool TorboxPairingServer::keyAccepted() const {
    return accepted_;
}

std::string TorboxPairingServer::acceptedKey() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return key_;
}

} // namespace pipensx
