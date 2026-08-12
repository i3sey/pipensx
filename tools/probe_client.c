/*
 * H8: can two processes on the console talk over TCP?
 *
 * If they can, the cmif IPC protocol in docs/plans/SYSMODULE_PLAN.md §3 is
 * unnecessary work — the daemon can serve the app and the Tesla overlay with
 * the HttpServer/WebServer that already exist in src/app.
 *
 * Run the probe sysmodule first, then launch this from hbmenu.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <switch.h>

#define PROBE_PORT 8099

static int try_connect(const char *label, u32 addr_be, u16 port)
{
    struct sockaddr_in addr;
    struct timeval tv;
    char buf[128];
    ssize_t got;
    int fd;

    printf("%s (%s:%u) ... ", label, inet_ntoa(*(struct in_addr *)&addr_be),
           (unsigned)port);
    consoleUpdate(NULL);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("socket errno=%d\n", errno);
        return 0;
    }

    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = addr_be;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("connect errno=%d\n", errno);
        close(fd);
        return 0;
    }

    send(fd, "GET /ping HTTP/1.0\r\n\r\n", 22, 0);
    got = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);

    if (got <= 0) {
        printf("connected, no reply errno=%d\n", errno);
        return 0;
    }
    buf[got] = '\0';
    printf("%s\n", strstr(buf, "pong") ? "OK (pong)" : "reply without pong");
    return 1;
}

int main(int argc, char **argv)
{
    PadState pad;
    u32 own_ip;
    int loopback_ok, lan_ok;

    (void)argc;
    (void)argv;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    printf("pipensx probe client (H8: cross-process TCP)\n\n");
    if (R_FAILED(socketInitializeDefault())) {
        printf("socketInitializeDefault failed\n");
    } else {
        own_ip = gethostid();
        loopback_ok = try_connect("loopback", htonl(INADDR_LOOPBACK), PROBE_PORT);
        lan_ok = try_connect("own LAN address", own_ip, PROBE_PORT);

        printf("\nverdict: loopback=%s lan=%s\n", loopback_ok ? "yes" : "no",
               lan_ok ? "yes" : "no");
        if (loopback_ok || lan_ok)
            printf("-> reuse WebServer as the daemon control plane\n");
        else
            printf("-> cmif server needed after all\n");
    }

    printf("\npress + to exit\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }

    socketExit();
    consoleExit(NULL);
    return 0;
}
