#pragma once

#include <stddef.h>
#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANTIZAPRET_MAX_ROUTES 8

typedef enum {
    ANTIZAPRET_ROUTE_DIRECT = 0,
    ANTIZAPRET_ROUTE_HTTP,
    ANTIZAPRET_ROUTE_HTTPS,
    ANTIZAPRET_ROUTE_SOCKS4,
    ANTIZAPRET_ROUTE_SOCKS5
} antizapret_route_type_t;

typedef struct {
    antizapret_route_type_t type;
    char address[256];
} antizapret_route_t;

void antizapret_init(const char *root_path);
void antizapret_set_enabled(int enabled);
int antizapret_is_enabled(void);
int antizapret_is_target_url(const char *url);
int antizapret_proxy_preferred(void);
void antizapret_note_proxy_success(void);

size_t antizapret_parse_pac(const char *pac, size_t pac_len,
                            antizapret_route_t *routes, size_t max_routes);
size_t antizapret_get_routes(antizapret_route_t *routes, size_t max_routes);
void antizapret_apply_route(CURL *curl, const antizapret_route_t *route);
const char *antizapret_route_name(const antizapret_route_t *route);
int antizapret_route_supported(const antizapret_route_t *route);
int antizapret_response_looks_blocked(const char *body, size_t body_len);

#ifdef __cplusplus
}
#endif
