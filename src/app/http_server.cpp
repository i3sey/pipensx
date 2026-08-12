#include "http_server.hpp"

extern "C" {
#include "../core/net.h"
#include "../core/util.h"
}

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace pipensx {

namespace {

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "";
    }
}

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

constexpr char kCanned503[] =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 18\r\n"
    "Retry-After: 2\r\n"
    "Connection: close\r\n\r\n"
    "{\"error\":\"busy\"}\r\n";

}  // namespace

std::string HttpRequest::header(const std::string& lowerName) const {
    auto it = headers.find(lowerName);
    return it == headers.end() ? "" : it->second;
}

std::string HttpRequest::queryParam(const std::string& name) const {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        size_t eq = query.find('=', pos);
        if (eq != std::string::npos && eq < amp) {
            if (query.compare(pos, eq - pos, name) == 0)
                return httpUrlDecode(query.substr(eq + 1, amp - eq - 1));
        } else if (query.compare(pos, amp - pos, name) == 0) {
            return "";
        }
        pos = amp + 1;
    }
    return "";
}

HttpResponse HttpResponse::text(int status, std::string body,
                                std::string contentType) {
    HttpResponse r;
    r.status = status;
    r.contentType = std::move(contentType);
    r.body = std::make_shared<const std::string>(std::move(body));
    return r;
}

HttpResponse HttpResponse::empty(int status) {
    HttpResponse r;
    r.status = status;
    return r;
}

std::string httpUrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit((unsigned char)in[i + 1]) &&
            std::isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out.push_back((char)strtol(hex, nullptr, 16));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

long httpParseHeaders(const std::string& data, size_t headerCap,
                      HttpRequest& out, int& errStatus) {
    size_t end = data.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (data.size() > headerCap) {
            errStatus = 431;
            return -1;
        }
        return 0;
    }
    if (end + 4 > headerCap) {
        errStatus = 431;
        return -1;
    }

    size_t lineEnd = data.find("\r\n");
    std::string requestLine = data.substr(0, lineEnd);
    size_t sp1 = requestLine.find(' ');
    size_t sp2 = requestLine.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) {
        errStatus = 400;
        return -1;
    }
    out.method = requestLine.substr(0, sp1);
    std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string version = requestLine.substr(sp2 + 1);
    if (version.compare(0, 7, "HTTP/1.") != 0) {
        errStatus = 400;
        return -1;
    }
    out.version = version;
    if (out.method != "GET" && out.method != "POST" && out.method != "PATCH" &&
        out.method != "HEAD") {
        errStatus = 405;
        return -1;
    }
    if (target.empty() || target[0] != '/') {
        errStatus = 400;
        return -1;
    }
    size_t q = target.find('?');
    if (q == std::string::npos) {
        out.path = httpUrlDecode(target);
        out.query.clear();
    } else {
        out.path = httpUrlDecode(target.substr(0, q));
        out.query = target.substr(q + 1);
    }

    out.headers.clear();
    size_t pos = lineEnd + 2;
    while (pos < end) {
        size_t eol = data.find("\r\n", pos);
        if (eol == std::string::npos || eol > end) eol = end;
        std::string line = data.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            errStatus = 400;
            return -1;
        }
        out.headers[toLower(trim(line.substr(0, colon)))] =
            trim(line.substr(colon + 1));
        pos = eol + 2;
    }
    return (long)(end + 4);
}

struct HttpServer::Conn {
    int fd = -1;
    enum State { ReadHeaders, ReadBody, Write, Sse } state = ReadHeaders;
    std::string inbuf;
    HttpRequest req;
    size_t bodyExpected = 0;
    bool isHead = false;
    bool isSse = false;
    std::string outHead;
    size_t outHeadOff = 0;
    std::shared_ptr<const std::string> outBody;
    size_t outBodyOff = 0;
    bool closeAfter = false;
    std::string sseBuf;
    uint64_t lastActivity = 0;
    bool dead = false;

    bool wantsWrite() const {
        return outHeadOff < outHead.size() ||
               (outBody && outBodyOff < outBody->size()) || !sseBuf.empty();
    }
};

HttpServer::HttpServer(Options options) : options_(options) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(uint16_t port, Handler handler, std::string& error) {
    if (running_.load()) {
        error = "already running";
        return false;
    }
    int fd = net_tcp_listen(port, 4);
    if (fd < 0) {
        error = "bind/listen failed";
        return false;
    }
    listenFd_ = fd;
    requestedPort_ = port;
    boundPort_ = net_local_port(fd);
    handler_ = std::move(handler);
    stopping_.store(false);
    running_.store(true);
    thread_ = std::thread(&HttpServer::loopMain, this);
    return true;
}

void HttpServer::stop() {
    if (!running_.load() && !thread_.joinable()) return;
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
    for (auto& c : conns_) net_close(c->fd);
    conns_.clear();
    sseClients_.store(0);
    if (listenFd_ >= 0) {
        net_close(listenFd_);
        listenFd_ = -1;
    }
    running_.store(false);
}

void HttpServer::broadcastSse(const std::string& frame) {
    for (auto& c : conns_) {
        if (!c->isSse || c->dead) continue;
        if (c->sseBuf.size() + frame.size() > options_.sseBufferCap) {
            c->dead = true;  // stalled client: back-pressure valve
            continue;
        }
        c->sseBuf += frame;
    }
}

bool HttpServer::ensureListener() {
    if (listenFd_ >= 0) return true;
    uint64_t now = nowMs();
    if (now - lastListenRetryMs_ < 1000) return false;
    lastListenRetryMs_ = now;
    int fd = net_tcp_listen(requestedPort_, 4);
    if (fd < 0) return false;
    listenFd_ = fd;
    boundPort_ = net_local_port(fd);
    log_msg("[http] listener restored on port %u\n", (unsigned)boundPort_);
    return true;
}

void HttpServer::acceptPending() {
    for (;;) {
        int fd = net_accept(listenFd_, nullptr);
        if (fd < 0) break;
        if (conns_.size() >= options_.maxConnections) {
            net_send(fd, (const uint8_t*)kCanned503, sizeof(kCanned503) - 1);
            net_close(fd);
            continue;
        }
        auto c = std::make_unique<Conn>();
        c->fd = fd;
        c->lastActivity = nowMs();
        conns_.push_back(std::move(c));
    }
}

void HttpServer::handleReadable(Conn& c) {
    uint8_t buf[4096];
    for (;;) {
        int n = net_recv(c.fd, buf, sizeof(buf));
        if (n > 0) {
            c.lastActivity = nowMs();
            if (c.state == Conn::Sse || c.state == Conn::Write) {
                // Data during a response (or SSE chatter): ignore the bytes;
                // for SSE a read of 0 below is how we learn the tab closed.
                continue;
            }
            c.inbuf.append((const char*)buf, (size_t)n);
            if (c.inbuf.size() > options_.headerCap + options_.bodyCap + 16) {
                c.dead = true;
                return;
            }
            continue;
        }
        if (n == 0) {
            c.dead = true;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        c.dead = true;
        return;
    }

    for (;;) {
        if (c.state == Conn::ReadHeaders) {
            int errStatus = 0;
            long consumed =
                httpParseHeaders(c.inbuf, options_.headerCap, c.req, errStatus);
            if (consumed == 0) return;
            if (consumed < 0) {
                respond(c, HttpResponse::text(
                               errStatus, std::string("{\"error\":\"") +
                                              statusText(errStatus) + "\"}"));
                c.closeAfter = true;
                return;
            }
            c.inbuf.erase(0, (size_t)consumed);
            c.isHead = c.req.method == "HEAD";
            if (!c.req.header("transfer-encoding").empty()) {
                respond(c, HttpResponse::text(501,
                                              "{\"error\":\"chunked body\"}"));
                c.closeAfter = true;
                return;
            }
            std::string cl = c.req.header("content-length");
            if (cl.empty()) {
                if (c.req.method == "POST" || c.req.method == "PATCH") {
                    respond(c, HttpResponse::text(
                                   411, "{\"error\":\"length required\"}"));
                    c.closeAfter = true;
                    return;
                }
                c.bodyExpected = 0;
            } else {
                char* endp = nullptr;
                unsigned long long v = strtoull(cl.c_str(), &endp, 10);
                if (!endp || *endp != '\0') {
                    respond(c, HttpResponse::text(
                                   400, "{\"error\":\"bad content-length\"}"));
                    c.closeAfter = true;
                    return;
                }
                if (v > options_.bodyCap) {
                    respond(c, HttpResponse::text(413,
                                                  "{\"error\":\"too large\"}"));
                    c.closeAfter = true;
                    return;
                }
                c.bodyExpected = (size_t)v;
            }
            c.state = Conn::ReadBody;
        }
        if (c.state == Conn::ReadBody) {
            if (c.inbuf.size() < c.bodyExpected) return;
            c.req.body = c.inbuf.substr(0, c.bodyExpected);
            c.inbuf.erase(0, c.bodyExpected);
            dispatch(c);
            if (c.state != Conn::ReadHeaders) return;
            // keep-alive with pipelined bytes already buffered: parse again
            if (c.inbuf.empty()) return;
            continue;
        }
        return;
    }
}

void HttpServer::dispatch(Conn& c) {
    HttpResponse resp;
    if (!handler_) {
        resp = HttpResponse::text(500, "{\"error\":\"no handler\"}");
    } else {
        try {
            resp = handler_(c.req);
        } catch (const std::exception& e) {
            log_msg("[http] handler exception: %s\n", e.what());
            resp = HttpResponse::text(500, "{\"error\":\"internal\"}");
        } catch (...) {
            resp = HttpResponse::text(500, "{\"error\":\"internal\"}");
        }
    }
    if (resp.sse && sseClients_.load() >= options_.maxSseClients) {
        resp = HttpResponse::text(503, "{\"error\":\"sse slots busy\"}");
        resp.extraHeaders.push_back({"Retry-After", "5"});
    }
    respond(c, resp);
}

void HttpServer::respond(Conn& c, const HttpResponse& resp) {
    bool reqKeepAlive = true;
    std::string connHdr = toLower(c.req.header("connection"));
    if (c.req.version == "HTTP/1.0")
        reqKeepAlive = connHdr == "keep-alive";
    else
        reqKeepAlive = connHdr != "close";
    bool keepAlive = reqKeepAlive && !resp.close && !c.closeAfter && !resp.sse;

    std::string head = "HTTP/1.1 " + std::to_string(resp.status) + " " +
                       statusText(resp.status) + "\r\n";
    head += "Content-Type: " + resp.contentType + "\r\n";
    size_t bodyLen = resp.body ? resp.body->size() : 0;
    if (resp.sse) {
        head += "Cache-Control: no-cache\r\n";
    } else if (resp.status != 204 && resp.status != 304) {
        head += "Content-Length: " + std::to_string(bodyLen) + "\r\n";
    }
    head += keepAlive || resp.sse ? "Connection: keep-alive\r\n"
                                  : "Connection: close\r\n";
    for (const auto& h : resp.extraHeaders)
        head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";

    c.outHead = std::move(head);
    c.outHeadOff = 0;
    bool suppressBody =
        c.isHead || resp.status == 204 || resp.status == 304;
    c.outBody = suppressBody ? nullptr : resp.body;
    c.outBodyOff = 0;
    c.isSse = resp.sse && !c.isHead;
    if (c.isSse) sseClients_.fetch_add(1);
    if (!keepAlive && !c.isSse) c.closeAfter = true;
    c.state = Conn::Write;
    pumpWrite(c);
}

void HttpServer::pumpWrite(Conn& c) {
    if (c.dead) return;
    while (c.outHeadOff < c.outHead.size()) {
        ssize_t n = net_send(c.fd, (const uint8_t*)c.outHead.data() + c.outHeadOff,
                             c.outHead.size() - c.outHeadOff);
        if (n < 0) {
            c.dead = true;
            return;
        }
        if (n == 0) return;  // socket full, POLLOUT re-arms
        c.outHeadOff += (size_t)n;
        c.lastActivity = nowMs();
    }
    while (c.outBody && c.outBodyOff < c.outBody->size()) {
        ssize_t n = net_send(c.fd, (const uint8_t*)c.outBody->data() + c.outBodyOff,
                             c.outBody->size() - c.outBodyOff);
        if (n < 0) {
            // Mid-body failures truncate the response the client sees —
            // leave a trace so on-device reports are diagnosable.
            log_msg("[http] body send failed at %zu/%zu: %s\n",
                    c.outBodyOff, c.outBody->size(), strerror(errno));
            c.dead = true;
            return;
        }
        if (n == 0) return;
        c.outBodyOff += (size_t)n;
        c.lastActivity = nowMs();
    }
    if (c.isSse) {
        while (!c.sseBuf.empty()) {
            ssize_t n = net_send(c.fd, (const uint8_t*)c.sseBuf.data(),
                                 c.sseBuf.size());
            if (n < 0) {
                c.dead = true;
                return;
            }
            if (n == 0) return;
            c.sseBuf.erase(0, (size_t)n);
            c.lastActivity = nowMs();
        }
        c.state = Conn::Sse;
        return;
    }
    // response fully written
    if (c.closeAfter) {
        c.dead = true;
        return;
    }
    c.state = Conn::ReadHeaders;
    c.req = HttpRequest{};
    c.outHead.clear();
    c.outHeadOff = 0;
    c.outBody = nullptr;
    c.outBodyOff = 0;
    c.bodyExpected = 0;
    c.isHead = false;
    // Buffered pipelined bytes are picked up by handleReadable's own loop
    // (via dispatch) or by loopMain on the next pass. Calling it from here
    // instead put one stack frame on the pipeline per request — 5000 of them
    // in a single write overflowed the stack.
}

void HttpServer::loopMain() {
    uint64_t lastTick = nowMs();
    std::vector<struct pollfd> pfds;
    while (!stopping_.load()) {
        ensureListener();
        pfds.clear();
        size_t listenerIdx = (size_t)-1;
        if (listenFd_ >= 0) {
            listenerIdx = pfds.size();
            pfds.push_back({listenFd_, POLLIN, 0});
        }
        size_t connBase = pfds.size();
        for (auto& c : conns_) {
            short ev = POLLIN;
            if (c->wantsWrite()) ev |= POLLOUT;
            pfds.push_back({c->fd, ev, 0});
        }

        int r = poll(pfds.data(), (nfds_t)pfds.size(), 250);
        uint64_t now = nowMs();

        if (r > 0) {
            if (listenerIdx != (size_t)-1) {
                short rev = pfds[listenerIdx].revents;
                if (rev & (POLLERR | POLLNVAL | POLLHUP)) {
                    // network went away (sleep/resume): re-listen lazily
                    log_msg("[http] listener error, will re-listen\n");
                    net_close(listenFd_);
                    listenFd_ = -1;
                } else if (rev & POLLIN) {
                    acceptPending();
                }
            }
            // acceptPending() above may have appended to conns_; those
            // entries have no pollfd this round, so stop at what was polled.
            size_t polled = pfds.size() - connBase;
            for (size_t i = 0; i < polled; ++i) {
                Conn& c = *conns_[i];
                short rev = pfds[connBase + i].revents;
                if (rev & (POLLERR | POLLNVAL)) {
                    c.dead = true;
                    continue;
                }
                if (rev & POLLOUT) pumpWrite(c);
                if (c.dead) continue;
                // !inbuf.empty(): a response finished draining and pipelined
                // requests are still buffered, with no new bytes to wake us.
                if ((rev & (POLLIN | POLLHUP)) || !c.inbuf.empty())
                    handleReadable(c);
            }
        }

        // timeout sweep. `now` predates this iteration's I/O, so an active
        // connection can carry lastActivity > now — clamp instead of letting
        // the unsigned diff underflow into an instant "timeout" (the same
        // trap the peer sweep hit once; it truncated every multi-MB body).
        for (auto& c : conns_) {
            if (c->dead) continue;
            uint64_t idle =
                now > c->lastActivity ? now - c->lastActivity : 0;
            switch (c->state) {
                case Conn::ReadHeaders:
                    if (c->inbuf.empty() && !c->wantsWrite()) {
                        if (idle > options_.idleTimeoutMs) c->dead = true;
                    } else if (idle > options_.readTimeoutMs) {
                        c->dead = true;
                    }
                    break;
                case Conn::ReadBody:
                    if (idle > options_.readTimeoutMs) c->dead = true;
                    break;
                case Conn::Write:
                    if (idle > options_.writeTimeoutMs) c->dead = true;
                    break;
                case Conn::Sse:
                    break;  // liveness enforced by the sseBuf cap
            }
        }

        for (size_t i = 0; i < conns_.size();) {
            if (conns_[i]->dead) {
                if (conns_[i]->isSse) sseClients_.fetch_sub(1);
                net_close(conns_[i]->fd);
                conns_.erase(conns_.begin() + (long)i);
            } else {
                ++i;
            }
        }

        if (now - lastTick >= options_.tickMs) {
            lastTick = now;
            if (tick_) tick_();
        }
    }
}

}  // namespace pipensx
