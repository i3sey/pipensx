/*
 * pipensx daemon — skeleton (docs/plans/SYSMODULE_PHASES.md phase 2, first
 * half). A homebrew sysmodule that boots, brings up the network and answers
 * GET /api/info over HTTP. No engine yet: if the process refuses to start,
 * the suspects are the NPDM and the service list, and mixing curl, zstd, ncm
 * and 15k lines of engine into that first launch buys nothing.
 *
 * Everything here that looks arbitrary was paid for by a hardware run of
 * src/probe — see docs/plans/SYSMODULE_PROBE_RESULTS.md §6.
 *
 * Build:   make daemon
 * Install: copy build-switch/daemon/atmosphere/... to the SD root, then
 *          enable "pipensx" in ovlSysmodules (no reboot).
 * Check:   curl -s http://<switch-ip>:8080/api/info
 */

#include <switch.h>

#include "app/http_server.hpp"

extern "C" {
#include "core/util.h"
}

#include <cstdio>
#include <memory>
#include <string>

/*
 * The number this skeleton exists to find: how much the SYSTEM pool will give
 * a homebrew process. The probe ran on 4 MiB with no engine and reported
 * 4.7 MiB in use; the engine needs an order of magnitude more, and stream
 * install needs more still (see the reserve floor in stream_ram_budget.cpp).
 *
 * Asked for as a *ceiling*, not a demand: a static array of this size is bss,
 * and the kernel refuses to create a process whose bss the pool cannot cover
 * — silently, with the module stuck "off" in ovlSysmodules and no crash
 * report. Claiming the heap at runtime and stepping down until the kernel
 * says yes removes that failure entirely and turns the ceiling into one
 * boot's worth of measurement instead of a bisection over console reboots.
 * /api/info reports asked vs granted.
 *
 * Measured on hardware: a 32 MiB-bss build stayed "off" while the probe
 * (4 MiB bss, same card, same loader, DBI closed) started normally. bss is
 * the difference, and it is the pool that refuses it.
 */
#ifndef PIPENSX_DAEMON_HEAP_MB
#define PIPENSX_DAEMON_HEAP_MB 64
#endif

/*
 * The ceiling on *this* heap, and why none of the obvious levers move it. It
 * is not the ceiling on the daemon's memory: see claimMemletMemory() below,
 * which gets 128 MiB from somewhere else entirely.
 *
 * Measured 2026-07-29, HOS 22.5.0 — every one of these gave the same 6 MiB:
 *
 *   ask 32 MiB / ask 128 MiB          memTotalBytes 6815744
 *   pool_partition 1 (Applet)         6815744
 *   pmshellBoostSystemMemoryResourceLimit(128 MiB), rc=0     6815744
 *   both together                     7077888
 *   every other homebrew sysmodule unloaded                  8388608
 *
 * memTotalBytes is not a per-process allowance, it is "what this process
 * holds plus what is still free in the shared System resource limit" — which
 * is why unloading the neighbours moved it and nothing else did.
 *
 * The limit is Atmosphère's, and it already does the thing that looks like
 * the fix: it shrinks the APPLET pool (eShop, album, web applet) and hands
 * that memory to SYSTEM so more custom sysmodules fit. Firmware 20.0.0 capped
 * that steal at 14 MB, down from 40 — for every custom sysmodule on the
 * console combined. So the boost has nothing left to take, which is exactly
 * how it returns rc=0 and changes nothing. There is no setting: Atmosphère
 * PR #697 proposed one and was rejected because pm sets resource limits
 * before set:sys exists.
 *
 * The boost code is gone; keeping a call that provably does nothing, holds
 * system-wide state past process exit and costs a pm:shell capability was the
 * worse trade. (It is not quite a no-op — on 5.0.0+ pm implements it as
 * SetUnsafeLimit, which opens svcMapPhysicalMemoryUnsafe against the
 * Application pool rather than growing the System limit. That is a second way
 * in, unexplored, and worse than memlet's: it takes from the pool games run
 * in, and cannot be raised while one is running.)
 */

/* svcSetHeapSize works in 2 MiB units and rejects anything else. */
#define DAEMON_HEAP_STEP (2u * 1024 * 1024)
#define DAEMON_HEAP_MIN  (2u * 1024 * 1024)

#define DAEMON_PORT     8080
#define DAEMON_LOG_PATH "sdmc:/switch/pipensx/daemon.log"

extern "C" {

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
u32 __nx_time_service_type = TimeServiceType_System;

extern char* fake_heap_start;
extern char* fake_heap_end;

/* Read by /api/info; the first of these is set before any service is up, so
 * there is nowhere to log from and every failure has to be reportable later. */
u64 g_heap_asked = (u64)PIPENSX_DAEMON_HEAP_MB * 1024 * 1024;
u64 g_heap_granted = 0;

/* Claim the largest heap the kernel will give, at most `want`, and publish it
 * to newlib: sbrk hands out whatever lies below fake_heap_end. Measured to
 * land on 6 MiB for any `want` above it. */
static u64 claimHeap(u64 want)
{
    for (u64 size = want; size > g_heap_granted; size -= DAEMON_HEAP_STEP) {
        void* base = NULL;
        if (R_SUCCEEDED(svcSetHeapSize(&base, size))) {
            fake_heap_start = (char*)base;
            fake_heap_end = (char*)base + size;
            g_heap_granted = size;
            return size;
        }
        if (size < DAEMON_HEAP_MIN + DAEMON_HEAP_STEP)
            break;
    }
    return g_heap_granted;
}

void __libnx_initheap(void)
{
    claimHeap(g_heap_asked);
}

} // extern "C"

namespace {

Result g_rcFs, g_rcSocket, g_rcTime, g_rcCsrng, g_rcNifm, g_rcSetsys;
bool g_sdmcMounted = false;
u32 g_hosMajor, g_hosMinor, g_hosMicro;

NifmRequest g_nifmRequest;
bool g_nifmRequestOpen = false;
Result g_rcNifmSubmit = 1;

/*
 * The way around the ceiling above, and it is not the System pool at all.
 *
 * `memlet` is an Atmosphere sysmodule (TID 0100000000000421) added in 1.9.0 so
 * ams.mitm could build romfs after firmware 20.0.0 took its memory away. It
 * declares pool_partition 1, so it belongs to the APPLET resource-limit group,
 * and its single command creates a shared memory out of that group's 507 MiB
 * and moves the handle to the caller. Mapping a shared memory costs the mapper
 * nothing, so the memory is charged to memlet and never touches the ~7-14 MiB
 * that all custom sysmodules on the console share.
 *
 * The applet pool is also not the pool games run in (that is Application), so
 * holding this does not shrink a running game.
 *
 * Command 65000: in u64 desired size, out granted size + move handle. memlet
 * caps at 128 MiB, requires MiB alignment and steps its own answer down 1 MiB
 * at a time until the pool covers it — so asking for the cap measures the
 * ceiling in a single boot and there is no knob worth adding here.
 *
 * Internal API with no stability promise, and the memory competes with
 * ams.mitm's romfs builder (large game mods). Every failure path below leaves
 * the daemon running on its own heap.
 *
 * Measured 2026-07-31, HOS 22.5.0, AMS with memlet present: asked 128 MiB,
 * granted 128 MiB, both end pages read back what was written. The whole ask
 * was met, so this is memlet's own cap and not the applet pool's — the pool
 * had more to give. Twenty-one times the System-pool heap, from a service
 * that was already installed.
 */
#define DAEMON_MEMLET_ASK (128u * 1024 * 1024)

Service g_memletSrv;
SharedMemory g_memletShmem;
Result g_rcMemlet = 1, g_rcMemletMap = 1;
u64 g_memletGranted = 0;
bool g_memletUsable = false;

void claimMemletMemory()
{
    g_rcMemlet = smGetService(&g_memletSrv, "memlet");
    if (R_FAILED(g_rcMemlet))
        return;

    Handle handle = INVALID_HANDLE;
    u64 asked = DAEMON_MEMLET_ASK, granted = 0;
    g_rcMemlet = serviceDispatchInOut(&g_memletSrv, 65000, asked, granted,
        .out_handle_attrs = { SfOutHandleAttr_HipcMove },
        .out_handles = &handle);
    if (R_FAILED(g_rcMemlet) || granted == 0)
        return;

    shmemLoadRemote(&g_memletShmem, handle, granted, Perm_Rw);
    g_rcMemletMap = shmemMap(&g_memletShmem);
    if (R_FAILED(g_rcMemletMap))
        return;
    g_memletGranted = granted;

    /* A mapping is not proof of backing pages. Touch both ends before
       reporting a number the engine would later size itself against. */
    volatile u8* p = (volatile u8*)shmemGetAddr(&g_memletShmem);
    p[0] = 0xA5;
    p[granted - 1] = 0x5A;
    g_memletUsable = (p[0] == 0xA5 && p[granted - 1] == 0x5A);
}

} // namespace

extern "C" void __appInit(void)
{
    static const SocketInitConfig socketConfig = {
        /*
         * A sysmodule cannot ask for the 12 sessions / sb_efficiency 8 that
         * borealis requests for the app. sb_efficiency 4 (not the 2 of
         * SYSMODULE_PLAN.md §2) and 8 sessions (not 4): with less, four
         * downloading sockets drained the bsd buffer pool and the control
         * connections got ENOBUFS and RST. A blocking recv() holds its
         * session, so the session count bounds how many sockets may sit in a
         * blocking call at once — the HTTP server needs one of them.
         */
        .tcp_tx_buf_size = 0x4000,
        .tcp_rx_buf_size = 0x8000,
        .tcp_tx_buf_max_size = 0x8000,
        .tcp_rx_buf_max_size = 0x20000,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 4,
        .num_bsd_sessions = 8,
        .bsd_service_type = BsdServiceType_System,
    };

    smInitialize();

    /*
     * Nothing caches the host version for us out here (borealis' NRO init does
     * it for the app). Without it libnx treats the firmware as 0.0.0 and every
     * "[3.0.0+]" guard fails with LibnxError_IncompatSysVer (0x4b59) — which
     * silently disables half the services this daemon will need.
     */
    g_rcSetsys = setsysInitialize();
    if (R_SUCCEEDED(g_rcSetsys)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            g_hosMajor = fw.major;
            g_hosMinor = fw.minor;
            g_hosMicro = fw.micro;
        }
        setsysExit();
    }

    g_rcFs = fsInitialize();
    if (R_SUCCEEDED(g_rcFs)) {
        /* Launched from boot2 the SD card may not be up yet. */
        for (int i = 0; i < 20; i++) {
            if (fsdevMountSdmc() == 0) {
                g_sdmcMounted = true;
                break;
            }
            svcSleepThread(500000000ULL);
        }
    }

    g_rcTime = timeInitialize();
    g_rcSocket = socketInitialize(&socketConfig);
    g_rcCsrng = csrngInitialize();
    g_rcNifm = nifmInitialize(NifmServiceType_System);
}

extern "C" void __appExit(void)
{
    if (g_nifmRequestOpen)
        nifmRequestClose(&g_nifmRequest);
    nifmExit();
    csrngExit();
    socketExit();
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

namespace {

/*
 * Submit a network request so the system keeps the interface up for us.
 * NOT SetKeptInSleep and NOT RegisterSocketDescriptor: those hand the socket
 * to the Wi-Fi chip's keep-alive (WoWLAN) — the connection survives sleep but
 * no traffic flows, so for downloading they are worse than useless
 * (SYSMODULE_PROBE_RESULTS.md §2).
 *
 * The request rolls back to state 1 across a sleep and has to be re-submitted
 * on wake; that belongs to phase 4, this only reports the live state.
 */
void submitNifmRequest()
{
    if (R_FAILED(g_rcNifm))
        return;
    Result rc = nifmCreateRequest(&g_nifmRequest, true);
    log_msg("[daemon] nifm CreateRequest rc=0x%x\n", rc);
    if (R_FAILED(rc))
        return;
    g_nifmRequestOpen = true;
    g_rcNifmSubmit = nifmRequestSubmit(&g_nifmRequest);
    log_msg("[daemon] nifm Submit rc=0x%x\n", g_rcNifmSubmit);
}

int nifmState()
{
    NifmRequestState state = NifmRequestState_Invalid;
    if (!g_nifmRequestOpen ||
        R_FAILED(nifmGetRequestState(&g_nifmRequest, &state)))
        return -1;
    return (int)state;
}

std::string infoJson()
{
    u64 memTotal = 0, memUsed = 0;
    svcGetInfo(&memTotal, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&memUsed, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{\"name\":\"pipensx-daemon\",\"version\":\"%s\","
        "\"firmware\":\"%u.%u.%u\","
        "\"heapAskedBytes\":%llu,\"heapGrantedBytes\":%llu,"
        "\"memTotalBytes\":%llu,\"memUsedBytes\":%llu,"
        "\"memletAskedBytes\":%llu,\"memletGrantedBytes\":%llu,"
        "\"memletUsable\":%s,"
        "\"sdmcMounted\":%s,\"nifmState\":%d,"
        "\"rc\":{\"setsys\":%u,\"fs\":%u,\"time\":%u,\"socket\":%u,"
        "\"csrng\":%u,\"nifm\":%u,\"nifmSubmit\":%u,"
        "\"memlet\":%u,\"memletMap\":%u}}",
        PIPENSX_VERSION,
        g_hosMajor, g_hosMinor, g_hosMicro,
        (unsigned long long)g_heap_asked, (unsigned long long)g_heap_granted,
        (unsigned long long)memTotal, (unsigned long long)memUsed,
        (unsigned long long)DAEMON_MEMLET_ASK,
        (unsigned long long)g_memletGranted,
        g_memletUsable ? "true" : "false",
        g_sdmcMounted ? "true" : "false", nifmState(),
        g_rcSetsys, g_rcFs, g_rcTime, g_rcSocket, g_rcCsrng, g_rcNifm,
        g_rcNifmSubmit, g_rcMemlet, g_rcMemletMap);
    return std::string(buf, n > 0 ? (size_t)n : 0);
}

pipensx::HttpResponse route(const pipensx::HttpRequest& req)
{
    if (req.path == "/api/info" && (req.method == "GET" || req.method == "HEAD"))
        return pipensx::HttpResponse::text(200, infoJson());
    return pipensx::HttpResponse::text(404, "{\"error\":\"not found\"}");
}

} // namespace

int main(int, char**)
{
    if (g_sdmcMounted)
        log_init(DAEMON_LOG_PATH);
    log_msg("[daemon] start heap=%llu/%llu fw=%u.%u.%u sdmc=%d fs=0x%x "
            "socket=0x%x time=0x%x csrng=0x%x nifm=0x%x setsys=0x%x\n",
            (unsigned long long)g_heap_granted,
            (unsigned long long)g_heap_asked,
            g_hosMajor, g_hosMinor, g_hosMicro,
            (int)g_sdmcMounted, g_rcFs, g_rcSocket, g_rcTime, g_rcCsrng,
            g_rcNifm, g_rcSetsys);

    submitNifmRequest();

    claimMemletMemory();
    log_msg("[daemon] memlet ask=%llu granted=%llu usable=%d rc=0x%x map=0x%x\n",
            (unsigned long long)DAEMON_MEMLET_ASK,
            (unsigned long long)g_memletGranted,
            (int)g_memletUsable, g_rcMemlet, g_rcMemletMap);

    /* HttpServer re-binds the listener itself once a second when it is gone
     * (ensureListener), which is exactly what waking from sleep needs. */
    pipensx::HttpServer server;
    std::string error;
    if (!server.start(DAEMON_PORT, route, error))
        log_msg("[daemon] http start failed: %s\n", error.c_str());
    else
        log_msg("[daemon] http listening on %u\n", (unsigned)server.boundPort());

    /* A sysmodule never returns. */
    for (;;) {
        svcSleepThread(1000000000ULL);
        log_flush();
    }
}
