#include "net.h"
#include "util.h"
#include <string.h>
#include <stdio.h>
#include <netinet/tcp.h>

int net_resolve(const char *host, uint16_t port, struct sockaddr_in *out) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    int r = getaddrinfo(host, port_str, &hints, &res);
    if (r != 0 || !res) {
        log_msg("[net] resolve '%s': %s\n", host, gai_strerror(r));
        return 0;
    }
    *out = *(struct sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    return 1;
}

int net_set_nonblock(socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

int net_set_tcp_receive_buffer(socket_t fd) {
    int receive_buffer_size = NET_TCP_RECEIVE_BUFFER_SIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size,
                   sizeof(receive_buffer_size)) == 0) {
        return 1;
    }
    int primary_error = errno;
    if (primary_error == ENOBUFS) {
        receive_buffer_size = NET_TCP_RECEIVE_BUFFER_FALLBACK_SIZE;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size,
                       sizeof(receive_buffer_size)) == 0) {
            log_msg("[net] set TCP receive buffer after handshake: %s; "
                    "using %d-byte fallback\n",
                    strerror(primary_error), receive_buffer_size);
            return 1;
        }
        int fallback_error = errno;
        log_msg("[net] set TCP receive buffer after handshake: %s; "
                "%d-byte fallback failed: %s\n",
                strerror(primary_error), receive_buffer_size,
                strerror(fallback_error));
        return 0;
    }
    log_msg("[net] set TCP receive buffer after handshake: %s\n",
            strerror(primary_error));
    return 0;
}

socket_t net_tcp_connect(const struct sockaddr_in *addr) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return INVALID_SOCK;
    net_set_nonblock(fd);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int r = connect(fd, (struct sockaddr*)addr, sizeof(*addr));
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return INVALID_SOCK;
    }
    return fd;
}

socket_t net_udp_socket(uint16_t local_port) {
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return INVALID_SOCK;
    net_set_nonblock(fd);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (local_port > 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(local_port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            log_msg("[net] bind UDP port %u: %s\n", local_port, strerror(errno));
            close(fd);
            return INVALID_SOCK;
        }
    }
    return fd;
}

socket_t net_tcp_listen(uint16_t port, int backlog) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return INVALID_SOCK;
    net_set_nonblock(fd);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_msg("[net] bind TCP port %u: %s\n", port, strerror(errno));
        close(fd);
        return INVALID_SOCK;
    }
    if (listen(fd, backlog) < 0) {
        log_msg("[net] listen port %u: %s\n", port, strerror(errno));
        close(fd);
        return INVALID_SOCK;
    }
    return fd;
}

socket_t net_accept(socket_t listener, struct sockaddr_in *peer) {
    struct sockaddr_in tmp;
    socklen_t len = sizeof(tmp);
    socket_t fd;
    do {
        fd = accept(listener, (struct sockaddr*)&tmp, &len);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return INVALID_SOCK;
    net_set_nonblock(fd);
    /* Accepted sockets serve the web companion's small HTTP/SSE frames;
       Nagle only adds latency there. Best-effort. */
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    if (peer) *peer = tmp;
    return fd;
}

uint16_t net_local_port(socket_t fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

void net_close(socket_t fd) {
    if (fd >= 0) close(fd);
}

ssize_t net_send(socket_t fd, const uint8_t *buf, size_t len) {
    /* Cap each send() call: libnx marshals the whole buffer through the bsd
       service transfer memory, and multi-megabyte writes (web catalog
       responses) can fail outright where a chunked series succeeds. Peer
       traffic never exceeds block size, so this only matters for the web
       server's large bodies. */
    const size_t max_chunk = 32 * 1024;
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > max_chunk)
            chunk = max_chunk;
        ssize_t n = send(fd, buf + sent, chunk,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
                         );
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        if (n == 0) break;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

int net_recv(socket_t fd, uint8_t *buf, size_t len) {
    ssize_t n;
    do {
        n = recv(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    return (int)n;
}
