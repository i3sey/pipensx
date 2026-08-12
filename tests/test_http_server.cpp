// Exercises the web companion's HTTP/1.1 protocol layer on the host: the
// parser byte-by-byte (caps, malformed requests, query decoding) and a real
// loopback server (keep-alive pipelining, POST bodies, connection/SSE caps,
// canned 503, HEAD). No Switch dependencies — plain BSD sockets.

#include "app/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

using namespace pipensx;

namespace {

int clientConnect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = connect(fd, (sockaddr*)&a, sizeof(a));
    assert(r == 0);
    return fd;
}

void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        assert(n > 0);
        off += (size_t)n;
    }
}

// Reads until buf contains needle (or the peer closes / recv times out).
std::string readUntil(int fd, const std::string& needle,
                      std::string carry = "") {
    std::string buf = std::move(carry);
    char tmp[2048];
    while (buf.find(needle) == std::string::npos) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
    }
    return buf;
}

// Reads one full response (headers + Content-Length body). Returns the whole
// raw text; carry keeps pipelined leftovers for the next call.
std::string readResponse(int fd, std::string& carry) {
    std::string buf = readUntil(fd, "\r\n\r\n", carry);
    size_t hdrEnd = buf.find("\r\n\r\n");
    assert(hdrEnd != std::string::npos);
    size_t bodyLen = 0;
    size_t clPos = buf.find("Content-Length: ");
    if (clPos != std::string::npos && clPos < hdrEnd)
        bodyLen = strtoul(buf.c_str() + clPos + 16, nullptr, 10);
    size_t total = hdrEnd + 4 + bodyLen;
    char tmp[2048];
    while (buf.size() < total) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
    }
    assert(buf.size() >= total);
    carry = buf.substr(total);
    return buf.substr(0, total);
}

void testParser() {
    const std::string req =
        "GET /api/tasks?pin=12%2034&x HTTP/1.1\r\n"
        "Host: switch\r\n"
        "X-Pipensx-Pin:  777 \r\n"
        "\r\n";
    // incremental: every strict prefix is "need more data"
    for (size_t i = 0; i + 1 < req.size(); ++i) {
        HttpRequest r;
        int err = 0;
        assert(httpParseHeaders(req.substr(0, i), 8192, r, err) == 0);
    }
    HttpRequest r;
    int err = 0;
    long consumed = httpParseHeaders(req + "tail", 8192, r, err);
    assert(consumed == (long)req.size());
    assert(r.method == "GET");
    assert(r.path == "/api/tasks");
    assert(r.version == "HTTP/1.1");
    assert(r.query == "pin=12%2034&x");
    assert(r.queryParam("pin") == "12 34");
    assert(r.queryParam("x").empty());
    assert(r.queryParam("nope").empty());
    assert(r.header("host") == "switch");
    assert(r.header("x-pipensx-pin") == "777");

    // header block over cap without terminator
    std::string big = "GET / HTTP/1.1\r\nX: " + std::string(9000, 'a');
    err = 0;
    assert(httpParseHeaders(big, 8192, r, err) == -1 && err == 431);

    // malformed request line
    err = 0;
    assert(httpParseHeaders("garbage\r\n\r\n", 8192, r, err) == -1 &&
           err == 400);
    err = 0;
    assert(httpParseHeaders("DELETE / HTTP/1.1\r\n\r\n", 8192, r, err) == -1 &&
           err == 405);
    err = 0;
    assert(httpParseHeaders("GET / SPDY/3\r\n\r\n", 8192, r, err) == -1 &&
           err == 400);
    printf("parser ok\n");
}

HttpServer::Options smallOptions() {
    HttpServer::Options o;
    o.maxConnections = 2;
    o.maxSseClients = 1;
    o.bodyCap = 256;
    o.tickMs = 50;
    return o;
}

void testServer() {
    HttpServer server(smallOptions());
    server.setTickCallback(
        [&server] { server.broadcastSse("data: tick\n\n"); });
    std::string error;
    bool ok = server.start(0, [](const HttpRequest& req) -> HttpResponse {
        if (req.path == "/echo")
            return HttpResponse::text(200, req.body, "text/plain");
        if (req.path == "/big") {
            // multi-megabyte body: exercises chunked net_send + POLLOUT
            // resumption (the catalog blob path that truncated on-device)
            std::string big(3 * 1024 * 1024, 'x');
            for (size_t i = 0; i < big.size(); i += 4096)
                big[i] = (char)('a' + (i / 4096) % 26);
            return HttpResponse::text(200, std::move(big), "text/plain");
        }
        if (req.path == "/events") {
            HttpResponse r = HttpResponse::text(200, "retry: 5000\n\n",
                                                "text/event-stream");
            r.sse = true;
            return r;
        }
        if (req.path == "/none") return HttpResponse::empty(204);
        return HttpResponse::text(404, "{\"error\":\"not found\"}");
    }, error);
    assert(ok);
    uint16_t port = server.boundPort();
    assert(port != 0);

    // keep-alive: two pipelined requests on one connection
    {
        int fd = clientConnect(port);
        sendAll(fd,
                "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello"
                "GET /none HTTP/1.1\r\n\r\n");
        std::string carry;
        std::string r1 = readResponse(fd, carry);
        assert(r1.find("200 OK") != std::string::npos);
        assert(r1.find("Connection: keep-alive") != std::string::npos);
        assert(r1.substr(r1.size() - 5) == "hello");
        std::string r2 = readResponse(fd, carry);
        assert(r2.find("204 No Content") != std::string::npos);
        close(fd);
    }

    // large body arrives complete and byte-exact
    {
        int fd = clientConnect(port);
        std::string carry;
        sendAll(fd, "GET /big HTTP/1.1\r\n\r\n");
        std::string resp = readResponse(fd, carry);
        assert(resp.find("200 OK") != std::string::npos);
        size_t bodyStart = resp.find("\r\n\r\n") + 4;
        std::string body = resp.substr(bodyStart);
        assert(body.size() == 3 * 1024 * 1024);
        bool exact = true;
        for (size_t i = 0; i < body.size(); ++i) {
            char expect = (i % 4096 == 0) ? (char)('a' + (i / 4096) % 26) : 'x';
            if (body[i] != expect) { exact = false; break; }
        }
        assert(exact);
        close(fd);
    }

    // HEAD: headers with Content-Length but no body bytes
    {
        int fd = clientConnect(port);
        sendAll(fd, "HEAD /missing HTTP/1.1\r\nConnection: close\r\n\r\n");
        std::string all = readUntil(fd, "\x01\x01");  // read to close
        assert(all.find("404 Not Found") != std::string::npos);
        assert(all.find("Content-Length: 21") != std::string::npos);
        assert(all.find("not found") == std::string::npos);
        close(fd);
    }

    // POST without Content-Length -> 411; chunked -> 501; oversized -> 413
    {
        int fd = clientConnect(port);
        std::string carry;
        sendAll(fd, "POST /echo HTTP/1.1\r\n\r\n");
        assert(readResponse(fd, carry).find("411") != std::string::npos);
        close(fd);

        fd = clientConnect(port);
        carry.clear();
        sendAll(fd,
                "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
        assert(readResponse(fd, carry).find("501") != std::string::npos);
        close(fd);

        fd = clientConnect(port);
        carry.clear();
        sendAll(fd, "POST /echo HTTP/1.1\r\nContent-Length: 500\r\n\r\n");
        assert(readResponse(fd, carry).find("413") != std::string::npos);
        close(fd);
    }

    // connection cap: 3rd concurrent connection gets the canned 503
    {
        int a = clientConnect(port);
        int b = clientConnect(port);
        // Hold both slots open. Finishing a request above frees a connection
        // before the third client connects and turns this into a timing race.
        int c = clientConnect(port);
        std::string resp = readUntil(c, "\r\n\r\n");
        assert(resp.find("503") != std::string::npos);
        close(c);
        close(a);
        close(b);
    }

    // SSE: initial payload + at least two broadcast frames; 2nd client 503
    {
        // The block above closed both connections, but the server only learns
        // that on its next poll — until then the cap is still full and this
        // client legitimately gets the canned 503. Retry rather than race it;
        // a cap that never reopens still fails, just on the assert below.
        int fd = -1;
        std::string got;
        for (int i = 0; i < 200; ++i) {
            fd = clientConnect(port);
            sendAll(fd, "GET /events HTTP/1.1\r\n\r\n");
            got = readUntil(fd, "retry: 5000");
            if (got.find("503") == std::string::npos) break;
            close(fd);
            fd = -1;
            usleep(10000);
        }
        assert(fd >= 0);
        assert(got.find("text/event-stream") != std::string::npos);
        assert(got.find("Content-Length") == std::string::npos);
        got = readUntil(fd, "data: tick\n\ndata: tick\n\n", got);
        assert(got.find("data: tick\n\ndata: tick\n\n") != std::string::npos);
        assert(server.sseClientCount() == 1);

        int fd2 = clientConnect(port);
        std::string carry;
        sendAll(fd2, "GET /events HTTP/1.1\r\n\r\n");
        assert(readResponse(fd2, carry).find("503") != std::string::npos);
        close(fd2);

        close(fd);
    }

    // deep pipelining on the default caps: every buffered request used to
    // cost a stack frame (handleReadable -> dispatch -> respond -> pumpWrite
    // -> handleReadable) before any of them was answered, so a single write
    // of a few thousand overflowed the stack. All of them must be answered.
    {
        HttpServer deep;  // default Options: ~2 MB of request buffering
        std::string err;
        assert(deep.start(
            0, [](const HttpRequest&) { return HttpResponse::empty(204); },
            err));
        int fd = clientConnect(deep.boundPort());
        int rcvbuf = 8 * 1024 * 1024;  // hold every reply, never block the server
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        constexpr int kCount = 5000;
        std::string blast;
        for (int i = 0; i < kCount; ++i) blast += "GET /none HTTP/1.1\r\n\r\n";
        sendAll(fd, blast);

        std::string got;
        char chunk[65536];
        int seen = 0;
        while (seen < kCount) {
            ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            got.append(chunk, (size_t)n);
            seen = 0;
            for (size_t p = got.find("204 No Content"); p != std::string::npos;
                 p = got.find("204 No Content", p + 1))
                ++seen;
        }
        assert(seen == kCount);
        close(fd);
        deep.stop();
    }

    server.stop();
    assert(!server.running());
    printf("server ok\n");
}

}  // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);
    testParser();
    testServer();
    printf("test_http_server: all ok\n");
    return 0;
}
