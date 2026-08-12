/*
 * pipensx sysmodule probe.
 *
 * Answers, on real hardware and in one run, the hypotheses that the whole
 * "app becomes a catalog, engine becomes a daemon" plan rests on
 * (docs/plans/SYSMODULE_PLAN.md, docs/plans/ESHOP_APPLET_PLAN.md §5):
 *
 *   H1  do homebrew sysmodule threads get CPU inside the half-awake window?
 *   H2  do bytes move over the network then, and on how many sockets?
 *   H3  does psc deliver sleep/wake, and can we ack without wedging sleep?
 *   H4  is bgtc:t reachable from a homebrew process at all?
 *   H5  do bsd / fsdev / csrng work outside an applet context?
 *   H6  does the load survive a running game?
 *   H7  what does a process like this cost in the SYSTEM memory pool?
 *   H8  can two processes on the console talk over TCP? (if yes, the cmif IPC
 *       protocol in SYSMODULE_PLAN.md §3 is unnecessary — the daemon can just
 *       run the WebServer that already exists)
 *
 * The torrent engine is deliberately NOT linked: mixing "bsd does not work in a
 * sysmodule" with "the piece buffer does not fit" answers neither.
 *
 * Results are read over TCP (curl http://<switch>:8099/) and mirrored to
 * sdmc:/switch/pipensx/probe-report.txt.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <switch.h>

#include "probe_report.h"

/*
 * 2 MiB. No engine and no piece buffers, so most of it is headroom for the bsd
 * transfer memory socketInitialize allocates from this heap — a socket init
 * that fails on memory would cost a whole hardware session, and the size is
 * logged so it can be subtracted from the H7 reading.
 */
#define PROBE_HEAP_SIZE     0x400000
#define PROBE_STACK_SIZE    0x4000
#define PROBE_REPORT_BUFFER (192 * 1024)
#define PROBE_CONFIG_PATH   "sdmc:/switch/pipensx/probe.ini"
#define PROBE_REPORT_PATH   "sdmc:/switch/pipensx/probe-report.txt"

/*
 * psc module id. Not one of the system's own (see PscPmModuleId in psc.h) —
 * homebrew has to claim an unused slot, and picking a live one would fight the
 * real owner over sleep acknowledgements.
 */
#define PROBE_PSC_MODULE_ID ((PscPmModuleId)0x7E)

/* Threads live on the system core so the probe never competes with a game. */
#define PROBE_THREAD_CORE 3
#define PROBE_THREAD_PRIO 0x3B

/* ---------------------------------------------------------------- sysmodule */

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
u32 __nx_time_service_type = TimeServiceType_System;

static char g_inner_heap[PROBE_HEAP_SIZE];

void __libnx_initheap(void)
{
    extern char *fake_heap_start;
    extern char *fake_heap_end;

    fake_heap_start = g_inner_heap;
    fake_heap_end = g_inner_heap + sizeof(g_inner_heap);
}

/* --------------------------------------------------------------- probe state */

static probe_config_t g_cfg;
static probe_ring_t g_ring;
static probe_log_t g_log;
static Mutex g_log_lock;
static char g_report[PROBE_REPORT_BUFFER];

static Result g_rc_fs, g_rc_socket, g_rc_time, g_rc_psc, g_rc_nifm, g_rc_csrng;
static Result g_rc_setsys, g_rc_psm;
static u8 g_hos_major, g_hos_minor, g_hos_micro;
static bool g_sdmc_mounted;

static PscPmModule g_psc_module;
static bool g_psc_ready;
static u8 g_psc_state = PscPmState_Awake;
static bool g_post_wake;

static NifmRequest g_nifm_request;
static bool g_nifm_request_open;
static bool g_kept_in_sleep_ok;
static bool g_kept_socket_registered;

static u64 g_rx_bytes[PROBE_NET_THREADS];
static u8 g_sock_alive;
/* Set by the sampler, read by the net threads. See load_in_sleep_only. */
static bool g_load_open = true;

static bool g_running = true;

static void probe_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void probe_logf(const char *fmt, ...)
{
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    mutexLock(&g_log_lock);
    probe_log_add(&g_log, line);
    mutexUnlock(&g_log_lock);
}

/* ------------------------------------------------------------------- bgtc */

/*
 * Not in libnx (only PscPmModuleId_Bgtc exists there), so this is raw cmif.
 *
 * The first hardware run guessed the shape and produced a false negative worth
 * writing down. `bgtc:t` is only a root interface: switchbrew's Glue services
 * page calls it nn::bgtc::ITaskServiceRoot on [11.0.0+] and gives it exactly one
 * command, 100 CreateTaskService — which is the single id the blind sweep found
 * answering there. Everything that matters (4 IsInHalfAwake, 102
 * WillDisconnectNetworkWhenEnteringSleep, 103 WillStayHalfAwakeInsteadSleep,
 * 200 EnableStayHalfAwake [12.0.0+]) lives on the object that command returns.
 *
 * `bgtc:sc` is a different interface with a different table —
 * nn::bgtc::IStateControlService: 1 GetState, 2 GetStateChangedEvent,
 * 3 NotifyEnteringHalfAwake, 4 NotifyLeavingHalfAwake. The first run polled
 * "cmd 4" once a second believing it was IsInHalfAwake, i.e. it told the state
 * service we were leaving half-awake, every second, right through the sleep
 * test. Only GetState is read-only, so only GetState is called from here.
 */
enum {
    BgtcCreateTaskService     = 100, /* root */
    BgtcNotifyTaskStarting    = 1,
    BgtcNotifyTaskFinished    = 2,
    BgtcIsInHalfAwake         = 4,
    BgtcNotifyClientName      = 5,
    BgtcIsInFullAwake         = 6,
    BgtcGetOperationMode      = 101,
    BgtcWillDisconnectNetwork = 102,
    BgtcWillStayHalfAwake     = 103,
    BgtcScheduleTask          = 11,
    BgtcEnableStayHalfAwake   = 200,
    BgtcScGetState            = 1,
};

static Service g_bgtc_root;
static Service g_bgtc_task; /* ITaskService, held open for the whole run */
static Service g_bgtc_sc;
static bool g_bgtc_task_ok;
static bool g_bgtc_sc_ok;
/* cmd 4 answered, so the sampler is allowed to poll it. */
static bool g_bgtc_half_awake_cmd_ok;

static void bgtc_task_close(void)
{
    if (g_bgtc_task_ok) {
        serviceClose(&g_bgtc_task);
        g_bgtc_task_ok = false;
    }
    if (serviceIsActive(&g_bgtc_root))
        serviceClose(&g_bgtc_root);
}

/*
 * EnableStayHalfAwake is a per-session state on the ITaskService object, so the
 * object is opened once and kept. A rejected command id still closes the session
 * (Result 0xf601), and every later call on it would fail for the wrong reason —
 * hence a reopen helper rather than one open at startup.
 */
static Result bgtc_task_open(void)
{
    Result rc;

    bgtc_task_close();
    rc = smGetService(&g_bgtc_root, "bgtc:t");
    if (R_FAILED(rc))
        return rc;
    rc = serviceDispatch(&g_bgtc_root, BgtcCreateTaskService,
                         .out_num_objects = 1,
                         .out_objects = &g_bgtc_task);
    g_bgtc_task_ok = R_SUCCEEDED(rc);
    if (!g_bgtc_task_ok)
        serviceClose(&g_bgtc_root);
    return rc;
}

/* Reopens on a hung-up session so one bad id does not poison the rest. */
static Result bgtc_task_out8(u32 cmd, u8 *out)
{
    u8 tmp = 0;
    Result rc;

    if (!g_bgtc_task_ok)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    rc = serviceDispatchOut(&g_bgtc_task, cmd, tmp);
    if (R_FAILED(rc))
        bgtc_task_open();
    else if (out)
        *out = tmp;
    return rc;
}

static Result bgtc_task_void(u32 cmd)
{
    Result rc;

    if (!g_bgtc_task_ok)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    rc = serviceDispatch(&g_bgtc_task, cmd);
    if (R_FAILED(rc))
        bgtc_task_open();
    return rc;
}

static Result bgtc_task_in8(u32 cmd, u8 value)
{
    Result rc;

    if (!g_bgtc_task_ok)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    rc = serviceDispatchIn(&g_bgtc_task, cmd, value);
    if (R_FAILED(rc))
        bgtc_task_open();
    return rc;
}

/* Re-reads the predicate; the whole lab is scored on this one value. */
static u8 bgtc_will_stay_half_awake(void)
{
    u8 will = 0;

    if (R_FAILED(bgtc_task_out8(BgtcWillStayHalfAwake, &will)))
        return 0xFF; /* call itself failed */
    return will;
}

/*
 * NotifyClientName wants the name as a HipcPointer buffer. Three other shapes
 * (MapAlias, a fixed 0x20 raw field, a raw u64) were tried on hardware and all
 * three answer 0xf601 — see SYSMODULE_PROBE_RESULTS.md §4.
 */
static Result bgtc_notify_client_name(const char *name)
{
    return serviceDispatch(&g_bgtc_task, BgtcNotifyClientName,
                           .buffer_attrs = { SfBufferAttr_HipcPointer |
                                             SfBufferAttr_In },
                           .buffers = { { name, strlen(name) + 1 } });
}

/* Takes an input of any width up to 8 bytes; only a void request is rejected. */
static Result bgtc_enable_stay_half_awake(void)
{
    return bgtc_task_in8(BgtcEnableStayHalfAwake, 1);
}

/*
 * Ф0 question 1 (SYSMODULE_PROBE_RESULTS.md §7): does the system hand out
 * half-awake windows on its own, or did our task order them? The measured run
 * called EnableStayHalfAwake, NotifyTaskStarting and ScheduleTask(60) together
 * and saw intervals of 20–113 s — uncomfortably close to the 60 it asked for,
 * and no way to tell which call was responsible.
 *
 * So each of the three is now its own ini key and this function only applies
 * what is asked for. The three runs that separate them:
 *
 *   stay=0 task=0 schedule=0    observe only — are there windows without us?
 *   stay=1 task=0 schedule=0    is "stay" alone enough?
 *   stay=1 task=0 schedule=30   do the intervals track the number we ask for?
 *
 * If the third moves the interval, the daemon can *order* windows and the
 * sleep-throughput ceiling is a knob rather than a constant. That is the whole
 * point of the question.
 *
 * IsInHalfAwake is polled by the sampler regardless of all three, so the
 * observe-only run still records windows: the session is opened and the
 * predicate probed here in every case.
 *
 * Every step reopens the ITaskService first, because a rejected id closes the
 * session and would otherwise carry a stale failure into the next call.
 */
static bool bgtc_setup(void)
{
    static const char *kName = "pipensx";
    u8 half = 0;

    if (!g_bgtc_task_ok)
        return false;

    probe_logf("bgtc setup: stay=%d task=%d schedule=%d baseline will=%u",
               g_cfg.bgtc_stay, g_cfg.bgtc_task, g_cfg.bgtc_schedule,
               bgtc_will_stay_half_awake());
    {
        u8 disconnect = 0, mode = 0;
        probe_logf("bgtc WillDisconnectNetworkWhenEnteringSleep rc=0x%x out=%u",
                   bgtc_task_out8(BgtcWillDisconnectNetwork, &disconnect),
                   disconnect);
        probe_logf("bgtc GetOperationMode rc=0x%x out=%u",
                   bgtc_task_out8(BgtcGetOperationMode, &mode), mode);
    }

    if (R_FAILED(bgtc_task_open()))
        return false;
    probe_logf("bgtc NotifyClientName rc=0x%x", bgtc_notify_client_name(kName));

    if (g_cfg.bgtc_schedule > 0) {
        u32 interval = (u32)g_cfg.bgtc_schedule;
        probe_logf("bgtc ScheduleTask(%u) rc=0x%x -> will=%u", interval,
                   serviceDispatchIn(&g_bgtc_task, BgtcScheduleTask, interval),
                   bgtc_will_stay_half_awake());
    }
    if (g_cfg.bgtc_stay)
        probe_logf("bgtc EnableStayHalfAwake rc=0x%x -> will=%u",
                   bgtc_enable_stay_half_awake(),
                   bgtc_will_stay_half_awake());
    if (g_cfg.bgtc_task)
        probe_logf("bgtc NotifyTaskStarting rc=0x%x -> will=%u",
                   bgtc_task_void(BgtcNotifyTaskStarting),
                   bgtc_will_stay_half_awake());

    /* Arms the sampler's per-second window flag. Must survive every branch
     * above being off, or the observe-only run records nothing. */
    if (R_SUCCEEDED(bgtc_task_out8(BgtcIsInHalfAwake, &half)))
        g_bgtc_half_awake_cmd_ok = true;

    return bgtc_will_stay_half_awake() == 1;
}

static void bgtc_scan(void)
{
    Result rc = bgtc_task_open();

    probe_logf("bgtc:t CreateTaskService rc=0x%x", rc);
    if (R_SUCCEEDED(rc))
        probe_logf("bgtc stay-half-awake granted=%d",
                   (int)bgtc_setup());

    rc = smGetService(&g_bgtc_sc, "bgtc:sc");
    g_bgtc_sc_ok = R_SUCCEEDED(rc);
    if (g_bgtc_sc_ok) {
        u8 state = 0;
        Result r = serviceDispatchOut(&g_bgtc_sc, BgtcScGetState, state);
        probe_logf("bgtc:sc GetState rc=0x%x state=%u", r, state);
        if (R_FAILED(r)) {
            serviceClose(&g_bgtc_sc);
            g_bgtc_sc_ok = false;
        }
    } else {
        probe_logf("bgtc:sc open rc=0x%x", rc);
    }
}

/* ------------------------------------------------------------------ helpers */

static u64 probe_rtc_seconds(void)
{
    u64 ts = 0;

    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &ts)))
        return 0;
    return ts;
}

static u64 probe_used_memory(void)
{
    u64 used = 0;

    if (R_FAILED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)))
        return 0;
    return used;
}

static int probe_connect(const char *host, u16 port)
{
    struct sockaddr_in addr;
    struct addrinfo hints, *res = NULL;
    int fd;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    /* Dotted-quad first: keeps DNS out of the experiment when we want it out. */
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        char portstr[8];
        snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
            return -1;
        memcpy(&addr, res->ai_addr, sizeof(addr));
        freeaddrinfo(res);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void probe_flush_report(void)
{
    FILE *f;
    size_t len;

    if (!g_sdmc_mounted)
        return;

    mutexLock(&g_log_lock);
    len = probe_report_format(&g_ring, &g_log, g_report, sizeof(g_report));
    mutexUnlock(&g_log_lock);

    f = fopen(PROBE_REPORT_PATH, "wb");
    if (!f)
        return;
    fwrite(g_report, 1, len, f);
    fclose(f);
}

/* -------------------------------------------------------------- net threads */

/*
 * One plain-HTTP GET per thread, bytes discarded, counter sampled at 1 Hz.
 * Thread 0's socket is the one registered with the KeptInSleep nifm request:
 * "Only 1 socket can be registered at a time with a NifmRequest" (socket.h), so
 * the difference between thread 0 and the rest across a sleep is the answer to
 * whether background downloading can use a peer swarm or a single connection.
 */
static Thread g_net_threads[PROBE_NET_THREADS];

static void net_thread_main(void *arg)
{
    const int index = (int)(uintptr_t)arg;
    /*
     * Two different things used to hide behind one ini key, and the difference
     * matters: nifmRequestSetKeptInSleep is a flag on the *request* ("keep the
     * network up across sleep"), while this registers a *socket* for the Wi-Fi
     * chip's WoWLAN keep-alive and is what kills thread 0. Registration used to
     * happen unconditionally, so `kept_in_sleep = 0` never actually turned it
     * off. Separate keys, because the useful combination for downloading is
     * very likely kept_in_sleep=1 with kept_socket=0.
     */
    const bool registered = (index == 0) && g_cfg.enable_kept_socket;
    /*
     * One buffer per thread, not one static shared by all of them: the bsd
     * service hands the caller's address to the socket driver, and concurrent
     * recv() calls naming the same address come back EFAULT. Static rather than
     * on-stack because the thread stacks are 16 KiB.
     */
    static char bufs[PROBE_NET_THREADS][16 * 1024];
    char *buf = bufs[index];
    char request[512];
    int failed_logged = 0;

    while (g_running) {
        int fd;

        /*
         * A run spanning a whole day would otherwise pull at line rate for
         * every waking hour, which has nothing to do with what such a run is
         * measuring. Gated, the load exists only inside half-awake windows —
         * the sleep behaviour of the working run is unchanged.
         */
        if (!__atomic_load_n(&g_load_open, __ATOMIC_RELAXED)) {
            svcSleepThread(1000000000ULL);
            continue;
        }

        fd = probe_connect(g_cfg.host, g_cfg.port);
        if (fd < 0) {
            /* One line per thread per outage, not one per retry: the last run
             * spent its whole 16 KiB log on this. */
            if (!failed_logged) {
                probe_logf("net[%d] connect failed errno=%d", index, errno);
                failed_logged = 1;
            }
            svcSleepThread(2000000000ULL);
            continue;
        }

        failed_logged = 0;

        if (registered && g_nifm_request_open) {
            /*
             * nifm.h: RegisterSocketDescriptor needs NifmRequestState_Available.
             * Right after Submit the request is still OnHold, which is why the
             * first attempt came back EIO — wait for it instead of giving up.
             */
            NifmRequestState state = NifmRequestState_Invalid;
            int rc = -1, tries;
            for (tries = 0; tries < 20; tries++) {
                if (R_FAILED(nifmGetRequestState(&g_nifm_request, &state)))
                    break;
                if (state == NifmRequestState_Available)
                    break;
                svcSleepThread(250000000ULL);
            }
            if (state == NifmRequestState_Available)
                rc = socketNifmRequestRegisterSocketDescriptor(&g_nifm_request, fd);
            probe_logf("net[%d] nifm register state=%d rc=%d errno=%d", index,
                       (int)state, rc, errno);
            g_kept_socket_registered = (rc == 0);
        }

        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
                 "User-Agent: pipensx-probe\r\n\r\n",
                 g_cfg.path, g_cfg.host);
        if (send(fd, request, strlen(request), 0) < 0) {
            probe_logf("net[%d] send failed errno=%d", index, errno);
            close(fd);
            svcSleepThread(2000000000ULL);
            continue;
        }

        __atomic_or_fetch(&g_sock_alive, (u8)(1u << index), __ATOMIC_RELAXED);
        for (;;) {
            ssize_t got;

            if (!__atomic_load_n(&g_load_open, __ATOMIC_RELAXED))
                break; /* window closed: drop the stream, reconnect in the next */
            got = recv(fd, buf, sizeof(bufs[index]), 0);
            if (got > 0) {
                __atomic_add_fetch(&g_rx_bytes[index], (u64)got, __ATOMIC_RELAXED);
                continue;
            }
            probe_logf("net[%d] stream ended got=%zd errno=%d rx=%llu", index, got,
                       errno, (unsigned long long)__atomic_load_n(
                                  &g_rx_bytes[index], __ATOMIC_RELAXED));
            break;
        }
        __atomic_and_fetch(&g_sock_alive, (u8)~(1u << index), __ATOMIC_RELAXED);

        if (registered && g_nifm_request_open)
            socketNifmRequestUnregisterSocketDescriptor(&g_nifm_request, fd);
        close(fd);
        if (!g_running)
            break;
        svcSleepThread(1000000000ULL);
    }
}

/* ------------------------------------------------------------ flush thread */

/*
 * The SD mirror gets its own thread. Writing it from the sampler froze the
 * sampler dead at the second flush whenever DBI held the card — and the sample
 * that matters most is taken while the console is asleep and fs is unavailable.
 * The sampler must never block on I/O; a stuck flush may only cost the mirror.
 */
static Thread g_flush_thread;
static bool g_flush_now;

static void flush_thread_main(void *arg)
{
    int ticks = 0;

    (void)arg;
    while (g_running) {
        svcSleepThread(1000000000ULL);
        if (++ticks < 30 && !g_flush_now)
            continue;
        ticks = 0;
        g_flush_now = false;
        probe_flush_report();
    }
}

/* --------------------------------------------------------- SD writer (E7) */

/*
 * E7: can we WRITE during a half-awake window, not just receive?
 *
 * It decides whether installing in sleep is possible at all. Nintendo's own
 * sleep downloads leave a game ready to launch, so fs and ncm evidently live
 * through half-awake for system processes — but nothing says a homebrew
 * sysmodule gets the same, and every earlier probe run only ever filled RAM.
 *
 * Deliberately dumb: append a fixed block to one scratch file, count the bytes,
 * truncate when it gets big. No ncm, no placeholder, no NAND — the question is
 * "does the filesystem answer", and mixing it with content-storage semantics
 * would answer neither. ponytail: if this comes back positive, the real work is
 * making the installer resumable, not making this test smarter.
 */
#define PROBE_SD_SCRATCH  "sdmc:/switch/pipensx/probe-scratch.bin"
#define PROBE_SD_BLOCK    (64 * 1024)
#define PROBE_SD_MAX      (256 * 1024 * 1024)

static u64 g_sd_bytes;
static Thread g_sd_thread;

static void sd_thread_main(void *arg)
{
    static char block[PROBE_SD_BLOCK];
    u64 written = 0;
    int logged_error = 0;

    (void)arg;
    memset(block, 0xA5, sizeof(block));

    while (g_running) {
        FILE *f;
        size_t n;

        if (!g_sdmc_mounted) {
            svcSleepThread(1000000000ULL);
            continue;
        }

        f = fopen(PROBE_SD_SCRATCH, written ? "ab" : "wb");
        if (!f) {
            if (!logged_error) {
                probe_logf("sd writer fopen failed errno=%d", errno);
                logged_error = 1;
            }
            svcSleepThread(1000000000ULL);
            continue;
        }
        logged_error = 0;

        n = fwrite(block, 1, sizeof(block), f);
        /*
         * Flush every block: buffered bytes that never reach the card would
         * make a sleep window look productive when it was not.
         */
        fflush(f);
        fclose(f);

        written += n;
        __atomic_store_n(&g_sd_bytes, written, __ATOMIC_RELAXED);

        if (written >= PROBE_SD_MAX) {
            remove(PROBE_SD_SCRATCH);
            written = 0;
        }
    }
    remove(PROBE_SD_SCRATCH);
}

/* -------------------------------------------------------------- psc thread */

static Thread g_psc_thread;

static void psc_thread_main(void *arg)
{
    (void)arg;

    while (g_running) {
        PscPmState state;
        u32 flags = 0;

        if (R_FAILED(eventWait(&g_psc_module.event, 1000000000ULL)))
            continue;
        if (R_FAILED(pscPmModuleGetRequest(&g_psc_module, &state, &flags)))
            continue;

        g_psc_state = (u8)state;
        probe_logf("psc state=%d flags=%u tick=%llu rtc=%llu", (int)state, flags,
                   (unsigned long long)armGetSystemTick(),
                   (unsigned long long)probe_rtc_seconds());

        if (state == PscPmState_ReadySleep && g_cfg.enable_bgtc) {
            u8 will = 0;
            Result rc = bgtc_task_out8(BgtcWillStayHalfAwake, &will);
            probe_logf("bgtc WillStayHalfAwake at ReadySleep rc=0x%x will=%u", rc,
                       will);
        }
        if (state == PscPmState_Awake)
            g_post_wake = true;

        /*
         * Acknowledge immediately. A registered psc module that does not ack
         * holds up the whole sleep transition, and a wedged console is a much
         * worse outcome than a missed measurement.
         */
        pscPmModuleAcknowledge(&g_psc_module, state);

        if (state == PscPmState_Awake)
            g_flush_now = true;
    }
}

/* ----------------------------------------------------------- report server */

static Thread g_report_thread;

static void serve_client(int fd)
{
    char request[256];
    ssize_t got;
    size_t len;
    const char *body = g_report;
    char header[128];

    got = recv(fd, request, sizeof(request) - 1, 0);
    if (got <= 0)
        return;
    request[got] = '\0';

    if (strstr(request, "/ping")) {
        len = 4;
        body = "pong";
    } else {
        mutexLock(&g_log_lock);
        len = probe_report_format(&g_ring, &g_log, g_report, sizeof(g_report));
        mutexUnlock(&g_log_lock);
    }

    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n", len);
    send(fd, header, strlen(header), 0);

    {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(fd, body + sent, len - sent, 0);
            if (n <= 0)
                break;
            sent += (size_t)n;
        }
    }
}

/* Returns -1 on failure; logs why. */
static int report_listen(void)
{
    struct sockaddr_in addr;
    int listen_fd;
    int one = 1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        probe_logf("report listen socket failed errno=%d", errno);
        return -1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_cfg.report_port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, 2) < 0) {
        probe_logf("report bind/listen failed port=%u errno=%d",
                   (unsigned)g_cfg.report_port, errno);
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

static void report_thread_main(void *arg)
{
    int listen_fd = -1;
    int fails = 0;

    (void)arg;

    while (g_running) {
        int fd;

        /*
         * Sleep tears the listening socket down and does not give it back:
         * after the first E2 the port was simply refused while the process was
         * still sampling. So the listener is re-created rather than opened once
         * — the daemon will have to do exactly the same after every wake.
         */
        if (listen_fd < 0) {
            listen_fd = report_listen();
            if (listen_fd < 0) {
                svcSleepThread(2000000000ULL);
                continue;
            }
            probe_logf("report server listening on port %u",
                       (unsigned)g_cfg.report_port);
            fails = 0;
        }

        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (++fails > 10) {
                probe_logf("report accept failing (errno=%d), relistening", errno);
                close(listen_fd);
                listen_fd = -1;
            }
            svcSleepThread(200000000ULL);
            continue;
        }
        fails = 0;
        serve_client(fd);
        close(fd);
    }
    if (listen_fd >= 0)
        close(listen_fd);
}

/*
 * H8, cheap half: same process, so it only proves the bsd stack routes
 * loopback at all. The real cross-process answer comes from probe_client.nro.
 */
static void loopback_self_check(void)
{
    char buf[64];
    ssize_t got;
    int fd = probe_connect("127.0.0.1", g_cfg.report_port);

    if (fd < 0) {
        probe_logf("loopback in-process connect FAILED errno=%d", errno);
        return;
    }
    send(fd, "GET /ping\r\n\r\n", 13, 0);
    got = recv(fd, buf, sizeof(buf) - 1, 0);
    if (got > 0) {
        buf[got] = '\0';
        probe_logf("loopback in-process OK reply=%s", strstr(buf, "pong") ? "pong" : buf);
    } else {
        probe_logf("loopback in-process connected but no reply errno=%d", errno);
    }
    close(fd);
}

/* -------------------------------------------------------------------- init */

void __appInit(void)
{
    static const SocketInitConfig socket_config = {
        /* Compressed config from SYSMODULE_PLAN.md §2: a sysmodule cannot ask
         * for the 12 sessions / sb_efficiency 8 that borealis requests for the
         * app in switch_wrapper.c. */
        .tcp_tx_buf_size = 0x4000,
        .tcp_rx_buf_size = 0x8000,
        .tcp_tx_buf_max_size = 0x8000,
        .tcp_rx_buf_max_size = 0x20000,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0xA500,
        /*
         * 4, not 2. With sb_efficiency 2 the four download sockets drained the
         * bsd buffer pool and every further socket failed: the in-process
         * loopback check returned ENOBUFS and the report server's accepted
         * connections were reset. The plan's "compressed" socket config (§2)
         * does not leave room for a control channel alongside the transfers.
         */
        .sb_efficiency = 4,
        /*
         * 8, not 4. libnx hands each socket a session round-robin, and a
         * blocking recv() occupies its session: with 4 sessions and 4 download
         * threads the report server never got one and its connections were
         * reset. This is a real constraint for the daemon — the session count
         * bounds how many sockets can sit in a blocking call at once.
         */
        .num_bsd_sessions = 8,
        .bsd_service_type = BsdServiceType_System,
    };
    int i;

    smInitialize();

    /*
     * Nothing sets the cached host version for us here (borealis' NRO init does
     * it for the app). Without it libnx treats the firmware as 0.0.0 and every
     * "[3.0.0+]" guard fails with LibnxError_IncompatSysVer — which silently
     * disables exactly the calls this probe exists to test
     * (nifmRequestSetKeptInSleep, RegisterSocketDescriptor).
     */
    g_rc_setsys = setsysInitialize();
    if (R_SUCCEEDED(g_rc_setsys)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            g_hos_major = fw.major;
            g_hos_minor = fw.minor;
            g_hos_micro = fw.micro;
        }
        setsysExit();
    }

    g_rc_fs = fsInitialize();
    if (R_SUCCEEDED(g_rc_fs)) {
        /* Launched from boot2 the SD card may not be up yet. */
        for (i = 0; i < 20; i++) {
            if (fsdevMountSdmc() == 0) {
                g_sdmc_mounted = true;
                break;
            }
            svcSleepThread(500000000ULL);
        }
    }

    g_rc_time = timeInitialize();
    g_rc_socket = socketInitialize(&socket_config);
    g_rc_csrng = csrngInitialize();
    g_rc_nifm = nifmInitialize(NifmServiceType_System);
    g_rc_psc = pscmInitialize();
    g_rc_psm = psmInitialize();
}

void __appExit(void)
{
    pscmExit();
    nifmExit();
    csrngExit();
    socketExit();
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

static void load_config(void)
{
    static char text[4096];
    FILE *f;
    size_t len;

    probe_config_defaults(&g_cfg);
    if (!g_sdmc_mounted)
        return;
    f = fopen(PROBE_CONFIG_PATH, "rb");
    if (!f)
        return;
    len = fread(text, 1, sizeof(text), f);
    fclose(f);
    probe_config_parse(&g_cfg, text, len);
}

static void start_services(void)
{
    Result rc;
    int i;

    probe_logf("probe start tick=%llu tickfreq=%llu rtc=%llu",
               (unsigned long long)armGetSystemTick(),
               (unsigned long long)armGetSystemTickFreq(),
               (unsigned long long)probe_rtc_seconds());
    probe_logf("init fs=0x%x sdmc=%d socket=0x%x time=0x%x csrng=0x%x nifm=0x%x "
               "psc=0x%x psm=0x%x",
               g_rc_fs, (int)g_sdmc_mounted, g_rc_socket, g_rc_time, g_rc_csrng,
               g_rc_nifm, g_rc_psc, g_rc_psm);
    probe_logf("hosversion setsys=0x%x fw=%u.%u.%u", g_rc_setsys, g_hos_major,
               g_hos_minor, g_hos_micro);
    probe_logf("config host=%s:%u path=%s threads=%d psc=%d bgtc=%d kept=%d "
               "kept_socket=%d",
               g_cfg.host, (unsigned)g_cfg.port, g_cfg.path, g_cfg.net_threads,
               g_cfg.enable_psc, g_cfg.enable_bgtc, g_cfg.enable_kept_in_sleep,
               g_cfg.enable_kept_socket);
    /* H7 needs the probe's own static cost to be subtractable from the reading. */
    probe_logf("static heap=%zu ring=%zu report=%zu", sizeof(g_inner_heap),
               sizeof(g_ring), sizeof(g_report));

    if (g_cfg.enable_bgtc)
        bgtc_scan();

    if (R_SUCCEEDED(g_rc_nifm)) {
        rc = nifmCreateRequest(&g_nifm_request, true);
        probe_logf("nifm CreateRequest rc=0x%x", rc);
        if (R_SUCCEEDED(rc)) {
            NifmRequestState state = NifmRequestState_Invalid;
            g_nifm_request_open = true;
            nifmGetRequestState(&g_nifm_request, &state);
            probe_logf("nifm state before SetKeptInSleep=%d (want %d)", (int)state,
                       (int)NifmRequestState_Unknown1);
            if (g_cfg.enable_kept_in_sleep) {
                /* Must be set before submit: nifm.h requires state Unknown1. */
                rc = nifmRequestSetKeptInSleep(&g_nifm_request, true);
                g_kept_in_sleep_ok = R_SUCCEEDED(rc);
                probe_logf("nifm SetKeptInSleep rc=0x%x", rc);
            }
            rc = nifmRequestSubmit(&g_nifm_request);
            probe_logf("nifm Submit rc=0x%x", rc);
        }
    }

    if (g_cfg.enable_psc && R_SUCCEEDED(g_rc_psc)) {
        rc = pscmGetPmModule(&g_psc_module, PROBE_PSC_MODULE_ID, NULL, 0, true);
        g_psc_ready = R_SUCCEEDED(rc);
        probe_logf("psc GetPmModule id=0x%x rc=0x%x", PROBE_PSC_MODULE_ID, rc);
        if (g_psc_ready &&
            R_SUCCEEDED(threadCreate(&g_psc_thread, psc_thread_main, NULL, NULL,
                                     PROBE_STACK_SIZE, PROBE_THREAD_PRIO,
                                     PROBE_THREAD_CORE)))
            threadStart(&g_psc_thread);
    }

    if (R_SUCCEEDED(threadCreate(&g_report_thread, report_thread_main, NULL, NULL,
                                 PROBE_STACK_SIZE, PROBE_THREAD_PRIO,
                                 PROBE_THREAD_CORE)))
        threadStart(&g_report_thread);

    if (R_SUCCEEDED(threadCreate(&g_flush_thread, flush_thread_main, NULL, NULL,
                                 PROBE_STACK_SIZE, PROBE_THREAD_PRIO,
                                 PROBE_THREAD_CORE)))
        threadStart(&g_flush_thread);

    if (g_cfg.enable_sd_writer &&
        R_SUCCEEDED(threadCreate(&g_sd_thread, sd_thread_main, NULL, NULL,
                                 PROBE_STACK_SIZE, PROBE_THREAD_PRIO,
                                 PROBE_THREAD_CORE)))
        threadStart(&g_sd_thread);

    /*
     * Before the download threads, not after: the first run made this check
     * while four sockets were saturating the buffer pool, so its ENOBUFS said
     * more about buffers than about whether loopback routes at all.
     */
    if (g_cfg.enable_loopback_check) {
        svcSleepThread(500000000ULL);
        loopback_self_check();
    }
    probe_logf("memory after sockets used=%llu",
               (unsigned long long)probe_used_memory());

    for (i = 0; i < g_cfg.net_threads; i++) {
        if (R_SUCCEEDED(threadCreate(&g_net_threads[i], net_thread_main,
                                     (void *)(uintptr_t)i, NULL, PROBE_STACK_SIZE,
                                     PROBE_THREAD_PRIO, PROBE_THREAD_CORE)))
            threadStart(&g_net_threads[i]);
        else
            probe_logf("net[%d] threadCreate failed", i);
    }
}

int main(int argc, char **argv)
{
    u64 elapsed = 0;

    (void)argc;
    (void)argv;

    mutexInit(&g_log_lock);
    probe_ring_reset(&g_ring);
    probe_log_reset(&g_log);

    load_config();
    start_services();

    /* Startup result codes must outlive a day of window lines. */
    probe_log_pin(&g_log);

    /* The sampler is the main thread: 1 Hz, and the only writer of the ring. */
    unsigned half_awake_ticks = 0; /* 0 until the power state is logged once */
    bool half_awake_was = false;
    /*
     * Question 0 (2026-07-31): after some amount of use the console stops
     * granting half-awake windows at all, and only a reboot clears it. Finding
     * out how much use takes a run spanning a normal day, and the 1800-sample
     * ring is half an hour of that.
     *
     * The sampler only gets CPU when the system schedules us, so a jump in the
     * wall clock *is* the edge of a window. One log line per jump — how long we
     * ran, how many bytes it brought, how long we were gone — records the whole
     * day's window structure in the event log, which now evicts oldest-first.
     */
    u64 prev_rtc = 0, span_start = 0, span_rx = 0;
    unsigned gap_index = 0;

    while (g_running) {
        probe_sample_t sample;
        int i;

        memset(&sample, 0, sizeof(sample));
        sample.tick = armGetSystemTick();
        sample.rtc = probe_rtc_seconds();
        sample.heap_used = probe_used_memory();
        sample.psc_state = g_psc_state;
        sample.sock_alive = __atomic_load_n(&g_sock_alive, __ATOMIC_RELAXED);
        for (i = 0; i < PROBE_NET_THREADS; i++)
            sample.rx_bytes[i] = __atomic_load_n(&g_rx_bytes[i], __ATOMIC_RELAXED);
        sample.sd_bytes = __atomic_load_n(&g_sd_bytes, __ATOMIC_RELAXED);

        if (g_bgtc_half_awake_cmd_ok) {
            u8 out = 0;
            Result rc = bgtc_task_out8(BgtcIsInHalfAwake, &out);
            if (R_SUCCEEDED(rc) && out)
                sample.flags |= PROBE_FLAG_HALF_AWAKE;
            if (g_cfg.load_in_sleep_only)
                __atomic_store_n(&g_load_open, out != 0, __ATOMIC_RELAXED);

            /*
             * The predicate may only flip once the system is actually heading
             * for sleep (dimmed, idle, docked), which the startup setup cannot
             * see.
             *
             * Counting samples was wrong: during sleep the thread gets CPU only
             * inside a window, so "every 60 samples" is every 60 *scheduled
             * seconds*, and the runs of 2026-07-31 ended after 44-56 samples
             * with the line never printed once — losing the power state in
             * exactly the three runs that needed it. Log on every edge of the
             * predicate instead, plus the first sample, so the state is
             * recorded whether or not the process gets scheduled.
             */
            if (half_awake_ticks == 0 || half_awake_was != (out != 0)) {
                u32 charge = 0;
                PsmChargerType charger = PsmChargerType_Unconnected;
                u8 mode = 0;

                half_awake_was = (out != 0);
                half_awake_ticks = 1;
                /*
                 * bgtc.battery_threshold_stop is 20 in system settings, so a
                 * flat battery is a sufficient explanation for a refused
                 * half-awake window. Log the power state next to the predicate
                 * rather than arguing about it.
                 */
                psmGetBatteryChargePercentage(&charge);
                psmGetChargerType(&charger);
                bgtc_task_out8(BgtcGetOperationMode, &mode);
                probe_logf("bgtc half=%d will=%u mode=%u battery=%u charger=%d "
                           "rtc=%llu",
                           (int)half_awake_was, bgtc_will_stay_half_awake(),
                           mode, charge, (int)charger,
                           (unsigned long long)sample.rtc);
            }
        }
        if (g_kept_in_sleep_ok)
            sample.flags |= PROBE_FLAG_KEPT_SLEEP;
        if (g_kept_socket_registered)
            sample.flags |= PROBE_FLAG_SOCK_KEPT;
        if (g_nifm_request_open) {
            NifmRequestState state;
            if (R_SUCCEEDED(nifmGetRequestState(&g_nifm_request, &state)) &&
                state == NifmRequestState_Available)
                sample.flags |= PROBE_FLAG_NET_UP;
        }
        if (g_post_wake) {
            sample.flags |= PROBE_FLAG_POST_WAKE;
            g_post_wake = false;
        }

        probe_ring_push(&g_ring, &sample);

        {
            u64 rx_total = sample.sd_bytes;
            for (i = 0; i < PROBE_NET_THREADS; i++)
                rx_total += sample.rx_bytes[i];

            if (prev_rtc == 0) {
                span_start = sample.rtc;
                span_rx = rx_total;
            } else if (sample.rtc > prev_rtc + 4) {
                probe_logf("gap #%u ran=%llus rx=+%lluKiB then gone=%llus "
                           "half=%d rtc=%llu",
                           ++gap_index,
                           (unsigned long long)(prev_rtc - span_start),
                           (unsigned long long)((rx_total - span_rx) / 1024),
                           (unsigned long long)(sample.rtc - prev_rtc),
                           (int)half_awake_was,
                           (unsigned long long)sample.rtc);
                span_start = sample.rtc;
                span_rx = rx_total;
            }
            prev_rtc = sample.rtc;
        }

        elapsed++;
        if (g_cfg.run_seconds > 0 && elapsed >= (u64)g_cfg.run_seconds)
            break;

        svcSleepThread(1000000000ULL);
    }

    g_running = false;
    probe_flush_report();
    return 0;
}
