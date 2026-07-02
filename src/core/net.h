#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __SWITCH__
/* libnx BSD sockets */
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <poll.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <unistd.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <poll.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <unistd.h>
#endif

typedef int socket_t;
#define INVALID_SOCK (-1)
#define NET_TCP_RECEIVE_BUFFER_SIZE (256 * 1024)

/* Resolve hostname to IPv4 address; returns 1 on success */
int net_resolve(const char *host, uint16_t port, struct sockaddr_in *out);

/* Create nonblocking TCP socket and start connecting */
socket_t net_tcp_connect(const struct sockaddr_in *addr);

/* Create nonblocking UDP socket bound to local_port (0 = any) */
socket_t net_udp_socket(uint16_t local_port);

/* Set socket nonblocking */
int net_set_nonblock(socket_t fd);

/* Close a socket */
void net_close(socket_t fd);

/* Portable send/recv wrappers (handle EINTR) */
int net_send(socket_t fd, const uint8_t *buf, size_t len);
int net_recv(socket_t fd, uint8_t *buf, size_t len);
