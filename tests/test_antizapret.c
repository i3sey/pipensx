#include "core/antizapret.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_domains(void) {
    assert(antizapret_is_target_url("https://rutracker.org/forum"));
    assert(antizapret_is_target_url("http://bt4.t-ru.org/ann"));
    assert(antizapret_is_target_url("https://API.RUTRACKER.ORG/x"));
    assert(antizapret_is_target_url(
        "https://img-eshop.cdn.nintendo.net/i/icon.jpg"));
    assert(!antizapret_is_target_url("https://evilrutracker.org/"));
    assert(!antizapret_is_target_url("https://rutracker.org.example/"));
    assert(!antizapret_is_target_url(
        "https://img-eshop.cdn.nintendo.net.example/icon.jpg"));
    assert(antizapret_is_target_url("udp://bt4.t-ru.org:80/announce"));
}

static void test_pac_parser(void) {
    const char *pac =
        "function FindProxyForURL(url, host) {"
        " var proxy = \"HTTPS proxy-ssl.antizapret.prostovpn.org:3143; "
        "PROXY proxy-nossl.antizapret.prostovpn.org:29976; "
        "SOCKS5 socks.example:1080; DIRECT\";"
        " return proxy;"
        "}";
    antizapret_route_t routes[ANTIZAPRET_MAX_ROUTES];
    size_t count = antizapret_parse_pac(pac, strlen(pac), routes,
                                        ANTIZAPRET_MAX_ROUTES);
    assert(count == 4);
    assert(routes[0].type == ANTIZAPRET_ROUTE_HTTPS);
    assert(strcmp(routes[0].address,
                  "proxy-ssl.antizapret.prostovpn.org:3143") == 0);
    assert(routes[1].type == ANTIZAPRET_ROUTE_HTTP);
    assert(routes[2].type == ANTIZAPRET_ROUTE_SOCKS5);
    assert(routes[3].type == ANTIZAPRET_ROUTE_DIRECT);

    assert(antizapret_parse_pac("return \"DIRECT\";", 16, routes,
                                ANTIZAPRET_MAX_ROUTES) == 0);
    assert(antizapret_parse_pac("broken", 6, routes,
                                ANTIZAPRET_MAX_ROUTES) == 0);
}

static void test_block_pages(void) {
    const char *blocked =
        "<title>Доступ к запрашиваемому ресурсу ограничен</title>";
    assert(antizapret_response_looks_blocked(blocked, strlen(blocked)));
    assert(!antizapret_response_looks_blocked("<html>RuTracker</html>", 22));
}

static void test_builtin_routes(void) {
    antizapret_route_t routes[ANTIZAPRET_MAX_ROUTES];
    antizapret_init("/tmp/pipensx-antizapret-missing");
    antizapret_set_enabled(1);
    size_t count = antizapret_get_routes(routes, ANTIZAPRET_MAX_ROUTES);
    assert(count == 3);
    assert(routes[0].type == ANTIZAPRET_ROUTE_HTTP);
    assert(strcmp(routes[0].address,
                  "proxy-nossl.antizapret.prostovpn.org:29976") == 0);
    assert(routes[1].type == ANTIZAPRET_ROUTE_HTTPS);
    assert(routes[2].type == ANTIZAPRET_ROUTE_DIRECT);
    assert(antizapret_route_supported(&routes[0]));
}

int main(void) {
    test_domains();
    test_pac_parser();
    test_block_pages();
    test_builtin_routes();
    puts("antizapret tests passed");
    return 0;
}
