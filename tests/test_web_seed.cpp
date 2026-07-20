// Integration test for WebSeedSource: a minimal local HTTP server that honours
// Range requests, fetched piece-by-piece through the real curl path, then
// reassembled and compared to the served payload.
#include "../src/app/web_seed_source.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::vector<uint8_t> g_payload;

// Serve one Range request per accepted connection, then close.
void serve_connection(int fd) {
    char req[4096];
    size_t got = 0;
    // Read until end of headers.
    while (got < sizeof(req) - 1) {
        ssize_t n = recv(fd, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    req[got] = 0;

    uint64_t a = 0, b = 0;
    const char* r = strcasestr(req, "Range: bytes=");
    if (!r || sscanf(r + 13, "%llu-%llu", (unsigned long long*)&a,
                     (unsigned long long*)&b) != 2 ||
        b < a || b >= g_payload.size()) {
        const char* bad = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send(fd, bad, strlen(bad), 0);
        close(fd);
        return;
    }

    uint64_t len = b - a + 1;
    char head[256];
    int hn = snprintf(head, sizeof(head),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Content-Range: bytes %llu-%llu/%llu\r\n"
                      "Content-Length: %llu\r\n"
                      "Connection: close\r\n\r\n",
                      (unsigned long long)a, (unsigned long long)b,
                      (unsigned long long)g_payload.size(),
                      (unsigned long long)len);
    send(fd, head, (size_t)hn, 0);
    send(fd, g_payload.data() + a, (size_t)len, 0);
    close(fd);
}

} // namespace

int main() {
    // Payload: 3 pieces, last one short (2500 = 1000 + 1000 + 500).
    const uint64_t piece_length = 1000;
    const uint64_t total = 2500;
    const uint32_t num_pieces = 3;
    g_payload.resize(total);
    for (uint64_t i = 0; i < total; ++i)
        g_payload[i] = (uint8_t)(i * 31 + 7);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    assert(srv >= 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(srv, (sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(srv, 16) == 0);
    socklen_t alen = sizeof(addr);
    assert(getsockname(srv, (sockaddr*)&addr, &alen) == 0);
    uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> stop{false};
    std::thread server([&] {
        while (!stop.load()) {
            pollfd pfd{srv, POLLIN, 0};
            if (poll(&pfd, 1, 100) <= 0)
                continue;
            int fd = accept(srv, nullptr, nullptr);
            if (fd >= 0)
                serve_connection(fd);
        }
    });

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/file.bin", port);

    {
        pipensx::WebSeedSource src(url, "file.bin", piece_length, total,
                                   num_pieces, 3);
        for (uint32_t p = 0; p < num_pieces; ++p)
            assert(src.enqueue(p));
        // Enqueuing the same piece again is rejected.
        assert(!src.enqueue(0));

        std::vector<uint8_t> assembled(total, 0);
        std::vector<int> seen(num_pieces, 0);
        uint32_t drained = 0;
        for (int spins = 0; spins < 2000 && drained < num_pieces; ++spins) {
            pipensx::WebSeedSource::Completed c;
            if (!src.popCompleted(c)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            assert(c.ok);
            assert(c.piece < num_pieces);
            assert(!seen[c.piece]);
            seen[c.piece] = 1;
            uint64_t off = (uint64_t)c.piece * piece_length;
            uint64_t plen = (off + piece_length <= total) ? piece_length
                                                          : (total - off);
            assert(c.data.size() == plen);
            memcpy(assembled.data() + off, c.data.data(), c.data.size());
            ++drained;
        }
        assert(drained == num_pieces);
        assert(assembled == g_payload);
    }

    stop.store(true);
    server.join();
    close(srv);
    puts("web seed tests passed");
    return 0;
}
