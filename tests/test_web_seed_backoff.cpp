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
std::atomic<int> g_responseMode{1};

int responseStatus(uint64_t piece) {
    switch (g_responseMode.load()) {
    case 0:
        return 500;
    case 2:
        return piece < 2 ? 500 : 206;
    default:
        return 206;
    }
}

void serveOneBackoff(int fd) {
    char req[4096];
    size_t got = 0;
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

    unsigned long long parsedA = 0;
    unsigned long long parsedB = 0;
    const char* r = strcasestr(req, "Range: bytes=");
    if (!r || sscanf(r + 13, "%llu-%llu", &parsedA, &parsedB) != 2 ||
        parsedB < parsedA || parsedB >= g_payload.size()) {
        const char* bad = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send(fd, bad, strlen(bad), 0);
        close(fd);
        return;
    }
    uint64_t a = parsedA;
    uint64_t b = parsedB;

    int status = responseStatus(a / 1000);

    if (status != 206) {
        char head[128];
        int hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 %d Server Error\r\n"
                          "Content-Length: 0\r\nConnection: close\r\n\r\n",
                          status);
        send(fd, head, (size_t)hn, 0);
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

struct Server {
    int fd = -1;
    std::atomic<bool> stop{false};
    std::thread thread;

    uint16_t start() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        assert(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
        assert(listen(fd, 16) == 0);
        socklen_t alen = sizeof(addr);
        assert(getsockname(fd, (sockaddr*)&addr, &alen) == 0);

        stop.store(false);
        thread = std::thread([this] {
            while (!stop.load()) {
                pollfd pfd{fd, POLLIN, 0};
                if (poll(&pfd, 1, 100) <= 0)
                    continue;
                int cfd = accept(fd, nullptr, nullptr);
                if (cfd >= 0)
                    serveOneBackoff(cfd);
            }
        });
        return ntohs(addr.sin_port);
    }

    void shutdown() {
        stop.store(true);
        if (thread.joinable())
            thread.join();
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
};

} // namespace

int main() {
    const uint64_t pieceLength = 1000;
    const uint64_t total = 2500;
    const uint32_t numPieces = 3;
    g_payload.resize(total);
    for (uint64_t i = 0; i < total; ++i)
        g_payload[i] = (uint8_t)(i * 31 + 7);

    pipensx::WebSeedSource::BackoffPolicy fastBackoff;
    fastBackoff.maxConsecutiveFailures = 3;
    fastBackoff.baseBackoffSec = 0;
    fastBackoff.maxBackoffSec = 5;
    fastBackoff.deadReviveSec = 2;

    Server srv;
    g_responseMode.store(0);
    uint16_t port = srv.start();

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/file.bin", port);

    // ---- Test 1: consecutive failures lead to dead source ----
    {
        pipensx::WebSeedSource src(url, "file.bin", pieceLength, total,
                                   numPieces, 2, fastBackoff);

        assert(src.enqueue(0));
        assert(src.enqueue(1));
        assert(src.enqueue(2));

        int failures = 0;
        for (int spins = 0; spins < 500 && failures < 3; ++spins) {
            pipensx::WebSeedSource::Completed c;
            if (src.popCompleted(c)) {
                assert(!c.ok);
                failures++;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        assert(failures == 3);
        assert(src.isDead());
        assert(!src.enqueue(0));
        assert(!src.enqueue(1));
        assert(!src.enqueue(2));
    }

    // ---- Test 2: dead source revives after timeout ----
    {
        pipensx::WebSeedSource src(url, "file.bin", pieceLength, total,
                                   numPieces, 2, fastBackoff);

        assert(src.enqueue(0));
        assert(src.enqueue(1));
        assert(src.enqueue(2));

        int failures = 0;
        for (int spins = 0; spins < 500 && failures < 3; ++spins) {
            pipensx::WebSeedSource::Completed c;
            if (src.popCompleted(c)) {
                assert(!c.ok);
                failures++;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        assert(failures == 3);
        assert(src.isDead());

        // Switch server to success mode without changing the port.
        g_responseMode.store(1);

        // Still dead while within the cooldown window.
        assert(!src.enqueue(0));

        // Wait for dead timeout and a small margin.
        std::this_thread::sleep_for(std::chrono::seconds(3));
        assert(!src.isDead());

        // After timeout the source is alive again.
        assert(src.enqueue(0));
        assert(src.enqueue(1));
        assert(src.enqueue(2));

        uint32_t drained = 0;
        for (int spins = 0; spins < 500 && drained < numPieces; ++spins) {
            pipensx::WebSeedSource::Completed c;
            if (src.popCompleted(c)) {
                assert(c.ok);
                drained++;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        assert(drained == numPieces);
    }

    // ---- Test 3: backoff rejects immediate retry, then permits it later ----
    {
        pipensx::WebSeedSource::BackoffPolicy retryBackoff = fastBackoff;
        retryBackoff.baseBackoffSec = 1;
        g_responseMode.store(0);

        pipensx::WebSeedSource src(url, "file.bin", pieceLength, total,
                                   numPieces, 1, retryBackoff);

        assert(src.enqueue(0));
        bool sawFailure = false;
        for (int spins = 0; spins < 500 && !sawFailure; ++spins) {
            pipensx::WebSeedSource::Completed c;
            if (src.popCompleted(c)) {
                assert(!c.ok);
                sawFailure = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        assert(sawFailure);

        assert(!src.enqueue(0));
        std::this_thread::sleep_for(std::chrono::seconds(2));
        assert(src.enqueue(0));
    }

    // ---- Test 4: success resets the failure counter ----
    {
        pipensx::WebSeedSource src(url, "file.bin", pieceLength, total,
                                   numPieces, 1, fastBackoff);

        g_responseMode.store(2);
        assert(src.enqueue(0));
        assert(src.enqueue(1));
        assert(src.enqueue(2));

        int fails = 0;
        int successes = 0;
        for (int spins = 0; spins < 500 && fails < 2; ++spins) {
            pipensx::WebSeedSource::Completed c;
            if (src.popCompleted(c)) {
                if (c.ok)
                    successes++;
                else
                    fails++;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        assert(fails == 2);
        assert(successes == 0);

        // The next completed piece succeeds and resets the consecutive-failure
        // counter before it reaches the threshold.
        for (int spins = 0; spins < 500 && successes < 1; ++spins) {
            pipensx::WebSeedSource::Completed c;
            if (src.popCompleted(c)) {
                assert(c.ok);
                successes++;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        assert(successes == 1);
        assert(!src.isDead());
    }

    srv.shutdown();
    puts("web seed backoff tests passed");
    return 0;
}
