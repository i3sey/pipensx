#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pipensx {

// Minimal HTTP/1.1 server: one background thread, poll() over the listener
// plus a handful of keep-alive connections. Built for the Switch's tiny
// shared socket budget — hard caps on connections, header/body sizes and
// per-client buffering, everything nonblocking. Protocol layer only: routing
// and JSON live in the owner's handler.
//
// Server-Sent Events: a handler returns a response with sse=true; the
// connection then stays open receiving frames pushed via broadcastSse().
// The tick callback runs on the server thread roughly every tickMs — the
// owner builds state frames there and calls broadcastSse() (that call is
// only valid from the tick callback / handler, i.e. the server thread).

struct HttpRequest {
    std::string method;                          // "GET", "POST", "PATCH", "HEAD"
    std::string path;                            // decoded, without query
    std::string query;                           // raw query string ("" if none)
    std::string version;                         // "HTTP/1.1"
    std::string body;
    std::map<std::string, std::string> headers;  // keys lowercased

    std::string header(const std::string& lowerName) const;
    // Value of a query parameter, percent-decoded ("" if absent).
    std::string queryParam(const std::string& name) const;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::vector<std::pair<std::string, std::string>> extraHeaders;
    // Shared so large blobs (catalog, static files) are not copied per client.
    std::shared_ptr<const std::string> body;
    bool sse = false;    // body = initial SSE payload; connection stays open
    bool close = false;  // force Connection: close after this response

    static HttpResponse text(int status, std::string body,
                             std::string contentType = "application/json");
    static HttpResponse empty(int status);
};

// Parses one request's line + headers from data (which may hold more).
// Returns bytes consumed through the blank line, 0 if incomplete, or -1 on
// a malformed/oversized/unsupported request (errStatus = HTTP error code).
// headerCap bounds the header block size (431 when exceeded).
long httpParseHeaders(const std::string& data, size_t headerCap,
                      HttpRequest& out, int& errStatus);

std::string httpUrlDecode(const std::string& in);

class HttpServer {
public:
    struct Options {
        size_t maxConnections = 4;
        size_t maxSseClients = 2;
        size_t headerCap = 8 * 1024;
        size_t bodyCap = 2 * 1024 * 1024;
        size_t sseBufferCap = 64 * 1024;  // stalled SSE client dropped past this
        uint64_t tickMs = 1000;
        uint64_t readTimeoutMs = 10000;
        uint64_t writeTimeoutMs = 30000;
        uint64_t idleTimeoutMs = 60000;
    };

    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    using TickCallback = std::function<void()>;

    HttpServer() : HttpServer(Options()) {}
    explicit HttpServer(Options options);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Must be set before start(); runs on the server thread every ~tickMs.
    void setTickCallback(TickCallback cb) { tick_ = std::move(cb); }

    // Binds (port 0 = ephemeral) and starts the server thread.
    bool start(uint16_t port, Handler handler, std::string& error);
    // Closes everything and joins the thread. Safe to call repeatedly.
    void stop();

    bool running() const { return running_.load(); }
    uint16_t boundPort() const { return boundPort_; }

    // Appends one SSE frame to every streaming client. Server thread only.
    void broadcastSse(const std::string& frame);
    size_t sseClientCount() const { return sseClients_.load(); }

private:
    struct Conn;
    void loopMain();
    void handleReadable(Conn& c);
    void dispatch(Conn& c);
    void respond(Conn& c, const HttpResponse& resp);
    void pumpWrite(Conn& c);
    void acceptPending();
    bool ensureListener();

    Options options_;
    Handler handler_;
    TickCallback tick_;
    int listenFd_ = -1;
    uint16_t requestedPort_ = 0;
    uint16_t boundPort_ = 0;
    std::vector<std::unique_ptr<Conn>> conns_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> sseClients_{0};
    uint64_t lastListenRetryMs_ = 0;
};

}  // namespace pipensx
