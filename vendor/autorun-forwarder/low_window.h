#ifndef WINE_NX_LOW_WINDOW_H
#define WINE_NX_LOW_WINDOW_H

#include <stdint.h>

#define WINE_NX_GUEST_BASE UINT64_C(0x200000)
#define WINE_NX_NATIVE_BASE UINT64_C(0x100000000)
#define WINE_NX_HOST_LIMIT UINT64_C(0x8000000000)

static inline int wine_nx_is_low_window( uint64_t base, uint64_t size )
{
    return base == WINE_NX_GUEST_BASE && size == WINE_NX_HOST_LIMIT - base;
}

/* Called with the libnx virtual-memory lock held. */
int wine_nx_low_window_reserve(void);
int wine_nx_low_window_probe( void (*report)( const char * ) );

#endif
