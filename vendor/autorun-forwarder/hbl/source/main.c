#include <switch.h>
#include <string.h>
#include "../../low_window.h"

#define EXIT_DETECTION_STR "if this isn't replaced i will exit :)"

// this uses 16KiB less than nx-hbloader.
static char g_argv[2048] = {0};
// this can and will be modified by libnx if launched nro calls envSetNextLoad().
static char g_nextArgv[2048] = {0};
static char g_nextNroPath[FS_MAX_PATH] = {0};
// stores first launched nro argv + path.
static char g_defaultArgv[2048] = {0};
static char g_defaultNroPath[FS_MAX_PATH] = {0};

static const char g_noticeText[] = { "Autorun " VERSION };

static u64 g_nroSize = 0;
static NroHeader g_nroHeader = {0};

static enum {
    CodeMemoryUnavailable    = 0,
    CodeMemoryForeignProcess = BIT(0),
    CodeMemorySameProcess    = BIT(0) | BIT(1),
} g_codeMemoryCapability = CodeMemoryUnavailable;

static void*  g_heapAddr = {0};
static size_t g_heapSize = {0};

static Handle g_procHandle = {0};
static u128 g_userIdStorage = {0};
static u8 g_savedTls[0x100] = {0};

// Used by trampoline.s
u64 g_nroAddr = 0;
Result g_lastRet = 0;

void NX_NORETURN nroEntrypointTrampoline(const ConfigEntry* entries, u64 handle, u64 entrypoint);

static Result restoreMainThreadAffinity(void) {
    u64 core_mask = 0;
    Result rc = svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(rc))
        return rc;

    return svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, core_mask & 7);
}

static void fix_nro_path(char* path) {
    // hbmenu prefixes paths with sdmc: which fsFsOpenFile won't like
    if (!strncmp(path, "sdmc:/", 6)) {
        // memmove(path, path + 5, strlen(path)-5);
        memmove(path, path + 5, FS_MAX_PATH-5);
    }
}

// Credit to behemoth
// SOURCE: https://github.com/HookedBehemoth/nx-hbloader/commit/7f8000a41bc5e8a6ad96a097ef56634cfd2fabcb
static void NX_NORETURN selfExit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc))
        goto fail0;

    Service applet, proxy, self;

    rc = smGetService(&applet, "appletOE");
    if (R_FAILED(rc))
        goto fail1;

    const u32 cmd_id = 0;
    const u64 reserved = 0;

    // GetSessionProxy
    rc = serviceDispatchIn(&applet, cmd_id, reserved,
        .in_send_pid = true,
        .in_num_handles = 1,
        .in_handles = { g_procHandle },
        .out_num_objects = 1,
        .out_objects = &proxy,
    );
    if (R_FAILED(rc))
        goto fail2;

    // GetSelfController
    rc = serviceDispatch(&proxy, 1,
        .out_num_objects = 1,
        .out_objects = &self,
    );
    if (R_FAILED(rc))
        goto fail3;

    // Exit
    rc = serviceDispatch(&self, 0);

    serviceClose(&self);

fail3:
    serviceClose(&proxy);

fail2:
    serviceClose(&applet);

fail1:
    smExit();

fail0:
    if (R_SUCCEEDED(rc)) {
        while(1) svcSleepThread(86400000000000ULL);
        svcExitProcess();
        __builtin_unreachable();
    } else {
        diagAbortWithResult(rc);
    }
}

static u64 calculateMaxHeapSize(void) {
    u64 size = 0;
    u64 mem_available = 0, mem_used = 0;

    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);

    if (mem_available > mem_used+0x200000)
        size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
        size = 0x2000000*16;
    if (size > 0x6000000)
        size -= 0x6000000;

    return size;
}

static void setupHbHeap(void) {
    void* addr = NULL;
    u64 size = calculateMaxHeapSize();
    Result rc = svcSetHeapSize(&addr, size);

    if (R_FAILED(rc) || addr==NULL)
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 9));

    g_heapAddr = addr;
    g_heapSize = size;
}

static void procHandleReceiveThread(void* arg) {
    Handle session = (Handle)(uintptr_t)arg;
    Result rc;

    void* base = armGetTls();
    hipcMakeRequestInline(base);

    s32 idx = 0;
    rc = svcReplyAndReceive(&idx, &session, 1, INVALID_HANDLE, UINT64_MAX);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 15));

    HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.num_copy_handles != 1)
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 17));

    g_procHandle = r.data.copy_handles[0];
    svcCloseHandle(session);
}

static void getOwnProcessHandle(void) {
    Result rc;

    Handle server_handle, client_handle;
    rc = svcCreateSession(&server_handle, &client_handle, 0, 0);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 12));

    Thread t;
    u8* stack = g_heapAddr;
    rc = threadCreate(&t, &procHandleReceiveThread, (void*)(uintptr_t)server_handle, stack, 0x1000, 0x20, 0);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 10));

    rc = threadStart(&t);
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 13));

    hipcMakeRequestInline(armGetTls(),
        .num_copy_handles = 1,
    ).copy_handles[0] = CUR_PROCESS_HANDLE;

    svcSendSyncRequest(client_handle);
    svcCloseHandle(client_handle);

    threadWaitForExit(&t);
    threadClose(&t);
}

static bool isKernel5xOrLater(void) {
    u64 dummy = 0;
    Result rc = svcGetInfo(&dummy, InfoType_UserExceptionContextAddress, INVALID_HANDLE, 0);
    return R_VALUE(rc) != KERNELRESULT(InvalidEnumValue);
}

static bool isKernel4x(void) {
    u64 dummy = 0;
    Result rc = svcGetInfo(&dummy, InfoType_InitialProcessIdRange, INVALID_HANDLE, 0);
    return R_VALUE(rc) != KERNELRESULT(InvalidEnumValue);
}

static void getCodeMemoryCapability(void) {
    if (detectMesosphere()) {
        // Mesosphère allows for same-process code memory usage.
        g_codeMemoryCapability = CodeMemorySameProcess;
    } else if (isKernel5xOrLater()) {
        // On [5.0.0+], the kernel does not allow the creator process of a CodeMemory object
        // to use svcControlCodeMemory on itself, thus returning InvalidMemoryState (0xD401).
        // However the kernel can be patched to support same-process usage of CodeMemory.
        // We can detect that by passing a bad operation and observe if we actually get InvalidEnumValue (0xF001).
        Handle code;
        Result rc = svcCreateCodeMemory(&code, g_heapAddr, 0x1000);
        if (R_SUCCEEDED(rc)) {
            rc = svcControlCodeMemory(code, (CodeMapOperation)-1, 0, 0x1000, 0);
            svcCloseHandle(code);

            if (R_VALUE(rc) == KERNELRESULT(InvalidEnumValue))
                g_codeMemoryCapability = CodeMemorySameProcess;
            else
                g_codeMemoryCapability = CodeMemoryForeignProcess;
        }
    } else if (isKernel4x()) {
        // On [4.0.0-4.1.0] there is no such restriction on same-process CodeMemory usage.
        g_codeMemoryCapability = CodeMemorySameProcess;
    } else {
        // This kernel is too old to support CodeMemory syscalls.
        g_codeMemoryCapability = CodeMemoryUnavailable;
    }
}

static const romfs_file* findRomfsFile(const u8* table, u64 table_size, const romfs_dir* dir, const char* name) {
    u32 offset = dir->childFile;
    const size_t name_len = strlen(name);

    while (offset != UINT32_MAX) {
        if (offset > table_size || table_size - offset < sizeof(romfs_file))
            return NULL;

        const romfs_file* file = (const romfs_file*)(table + offset);
        if (file->nameLen > table_size - offset - sizeof(romfs_file))
            return NULL;

        if (file->nameLen == name_len && !memcmp(file->name, name, name_len))
            return file;

        offset = file->sibling;
    }

    return NULL;
}

static bool readRomfsFile(FsStorage* storage, const romfs_header* header, const u8* table, u64 table_size, const romfs_dir* dir, const char* name, void* out, size_t out_size) {
    const romfs_file* file = findRomfsFile(table, table_size, dir, name);
    if (!file || file->dataSize > out_size)
        return false;

    return R_SUCCEEDED(fsStorageRead(storage, header->fileDataOff + file->dataOff, out, file->dataSize));
}

static Result restoreProgramPath(NcmStorageId storage_id, u64 program_id, const char* path) {
    Result rc = smInitialize();
    if (R_FAILED(rc))
        return rc;

    rc = lrInitialize();
    if (R_SUCCEEDED(rc)) {
        LrLocationResolver resolver;
        rc = lrOpenLocationResolver(storage_id, &resolver);
        if (R_SUCCEEDED(rc)) {
            rc = lrLrRedirectProgramPath(&resolver, program_id, path);
            serviceClose(&resolver.s);
        }
        lrExit();
    }

    smExit();
    return rc;
}

struct nro_file {
    FsFile file;
    Result error;
};

static int readNroFile(void *context, uint64_t offset, void *data, size_t size) {
    struct nro_file *file = context;
    u64 read = 0;
    file->error = fsFileRead(&file->file, offset, data, size, FsReadOption_None, &read);
    return R_SUCCEEDED(file->error) && read == size;
}

static uint32_t nroU32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool nroImageSize(const unsigned char header[0x80], uint64_t file_size,
                         uint64_t heap_size, uint32_t *image_size) {
    uint64_t end = 0, size = nroU32(header + 0x18);
    uint64_t bss = nroU32(header + 0x38);
    if (memcmp(header + 0x10, "NRO0", 4) || size < 0x1000 || (size & 0xfff) ||
        size > file_size || size + ((bss + 0xfff) & ~UINT64_C(0xfff)) > heap_size) return false;
    for (unsigned int i = 0; i < 3; i++) {
        uint64_t offset = nroU32(header + 0x20 + i * 8);
        uint64_t bytes = nroU32(header + 0x24 + i * 8);
        if (offset != end || (bytes & 0xfff) || offset > size || bytes > size - offset) return false;
        end = offset + bytes;
    }
    if (end != size) return false;
    *image_size = size;
    return true;
}

void NX_NORETURN loadNro(void) {
    NroHeader* header = NULL;
    size_t rw_size = 0;
    Result rc = 0;

    memcpy((u8*)armGetTls() + 0x100, g_savedTls, 0x100);

    // check's if the homebrew replaced nro_path.
    // if so, load new nro, otherwise, exit.
    if (!strcmp(g_nextArgv, EXIT_DETECTION_STR)) {
        if (!strcmp(g_nextNroPath, g_defaultNroPath)) {
            selfExit();
        } else {
            // exited nro, now returning to default nro
            strcpy(g_nextNroPath, g_defaultNroPath);
            strcpy(g_nextArgv, g_defaultArgv);
        }
    }

    if (g_nroSize) {
        // checks if nro was previously mapped, if so, unmap
        header = &g_nroHeader;
        rw_size = (size_t)header->segments[2].size + header->bss_size;
        rw_size = (rw_size+0xFFF) & ~0xFFF;

        svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PreUnloadDll, g_nroAddr, g_nroSize);

        // .text
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[0].file_off, ((u64) g_heapAddr) + header->segments[0].file_off, header->segments[0].size);

        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 24));

        // .rodata
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[1].file_off, ((u64) g_heapAddr) + header->segments[1].file_off, header->segments[1].size);

        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 25));

       // .data + .bss
        rc = svcUnmapProcessCodeMemory(
            g_procHandle, g_nroAddr + header->segments[2].file_off, ((u64) g_heapAddr) + header->segments[2].file_off, rw_size);

        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 26));

        svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PostUnloadDll, g_nroAddr, g_nroSize);

        g_nroAddr = g_nroSize = 0;
    } else {
        // otherwise, this is the first time launching, read path / argv from romfs
        FsStorage s;
        romfs_header romfs_header;

        if (R_FAILED(rc = fsOpenDataStorageByCurrentProcess(&s))) {
            diagAbortWithResult(rc);
        }

        if (R_FAILED(rc = fsStorageRead(&s, 0, &romfs_header, sizeof(romfs_header)))) {
            diagAbortWithResult(rc);
        }

        u8 romfs_dirs[1024 * 2]; // should be 1 entry ("/")
        u8 romfs_files[1024 * 4];

        if (romfs_header.dirTableSize > sizeof(romfs_dirs) || romfs_header.fileTableSize > sizeof(romfs_files)) {
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, LibnxError_OutOfMemory));
        }

        if (R_FAILED(rc = fsStorageRead(&s, romfs_header.dirTableOff, romfs_dirs, romfs_header.dirTableSize))) {
            diagAbortWithResult(rc);
        }

        if (R_FAILED(rc = fsStorageRead(&s, romfs_header.fileTableOff, romfs_files, romfs_header.fileTableSize))) {
            diagAbortWithResult(rc);
        }

        const romfs_dir* dir = (const romfs_dir*)romfs_dirs;
        if (!readRomfsFile(&s, &romfs_header, romfs_files, romfs_header.fileTableSize, dir, "nextArgv", g_nextArgv, sizeof(g_nextArgv) - 1) ||
            !readRomfsFile(&s, &romfs_header, romfs_files, romfs_header.fileTableSize, dir, "nextNroPath", g_nextNroPath, sizeof(g_nextNroPath) - 1))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 44));

        char return_argv[sizeof(g_defaultArgv)] = {0};
        char return_nro_path[sizeof(g_defaultNroPath)] = {0};
        const bool has_return =
            readRomfsFile(&s, &romfs_header, romfs_files, romfs_header.fileTableSize, dir, "returnArgv", return_argv, sizeof(return_argv) - 1) &&
            readRomfsFile(&s, &romfs_header, romfs_files, romfs_header.fileTableSize, dir, "returnNroPath", return_nro_path, sizeof(return_nro_path) - 1);

        NcmStorageId redirect_storage = NcmStorageId_None;
        u64 redirect_program_id = 0;
        char redirect_path[FS_MAX_PATH] = {0};
        const bool has_redirect =
            readRomfsFile(&s, &romfs_header, romfs_files, romfs_header.fileTableSize, dir, "redirectStorage", &redirect_storage, sizeof(redirect_storage)) &&
            readRomfsFile(&s, &romfs_header, romfs_files, romfs_header.fileTableSize, dir, "redirectProgramId", &redirect_program_id, sizeof(redirect_program_id)) &&
            readRomfsFile(&s, &romfs_header, romfs_files, romfs_header.fileTableSize, dir, "redirectPath", redirect_path, sizeof(redirect_path) - 1);

        fsStorageClose(&s);

        if (has_redirect && R_FAILED(rc = restoreProgramPath(redirect_storage, redirect_program_id, redirect_path)))
            diagAbortWithResult(rc);

        if (has_return) {
            strcpy(g_defaultNroPath, return_nro_path);
            strcpy(g_defaultArgv, return_argv);
        } else {
            strcpy(g_defaultNroPath, g_nextNroPath);
            strcpy(g_defaultArgv, g_nextArgv);
        }

    }

    {
        // fix paths
        char fixedNextNroPath[FS_MAX_PATH];
        strcpy(fixedNextNroPath, g_nextNroPath);
        fix_nro_path(fixedNextNroPath);

        memcpy(g_argv, g_nextArgv, sizeof(g_argv));
        svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PreLoadDll, (uintptr_t)g_argv, sizeof(g_argv));

        uint8_t *nrobuf = (uint8_t*) g_heapAddr;
        NroStart*  start  = (NroStart*)  (nrobuf + 0);
        header = (NroHeader*) (nrobuf + sizeof(NroStart));

        FsFileSystem fs;
        if (R_FAILED(rc = fsOpenSdCardFileSystem(&fs))) {
            diagAbortWithResult(rc);
        }

        struct nro_file file = {0};
        if (R_FAILED(rc = fsFsOpenFile(&fs, fixedNextNroPath, FsOpenMode_Read, &file.file))) {
            diagAbortWithResult(rc);
        }

        s64 file_size = 0;
        unsigned char checked_header[0x80];
        uint32_t image_size;
        if (R_FAILED(rc = fsFileGetSize(&file.file, &file_size)))
            diagAbortWithResult(rc);
        if (file_size < sizeof(checked_header))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 6));
        if (!readNroFile(&file, 0, checked_header, sizeof(checked_header)) ||
            !nroImageSize(checked_header, file_size, g_heapSize, &image_size))
            diagAbortWithResult(R_FAILED(file.error) ? file.error : MAKERESULT(Module_HomebrewLoader, 6));
        if (!readNroFile(&file, 0, start, image_size))
            diagAbortWithResult(R_FAILED(file.error) ? file.error : MAKERESULT(Module_HomebrewLoader, 6));
        if (memcmp(start, checked_header, sizeof(checked_header)))
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 6));
        fsFileClose(&file.file);
        fsFsClose(&fs);
    }

    rw_size = (size_t)header->segments[2].size + header->bss_size;
    rw_size = (rw_size+0xFFF) & ~0xFFF;

    // Copy header to elsewhere because we're going to unmap it next.
    memcpy(&g_nroHeader, header, sizeof(g_nroHeader));
    header = &g_nroHeader;

    // Map code memory to a new randomized address
    virtmemLock();
    const size_t total_size = ((size_t)header->size + header->bss_size + 0xFFF) & ~(size_t)0xFFF;
    void* map_addr = virtmemFindCodeMemory(total_size, 0);
    u64 aslr_base = 0, aslr_size = 0;
    svcGetInfo(&aslr_base, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&aslr_size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
    if (wine_nx_is_low_window(aslr_base, aslr_size)) {
        for (unsigned int attempt = 0; map_addr && (uintptr_t)map_addr < WINE_NX_NATIVE_BASE && attempt < 64; ++attempt)
            map_addr = virtmemFindCodeMemory(total_size, 0);
        if ((uintptr_t)map_addr < WINE_NX_NATIVE_BASE)
            diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 18));
    }
    rc = svcMapProcessCodeMemory(g_procHandle, (u64)map_addr, (u64)g_heapAddr, total_size);
    virtmemUnlock();

    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 18));

    // .text
    rc = svcSetProcessMemoryPermission(
        g_procHandle, (u64)map_addr + header->segments[0].file_off, header->segments[0].size, Perm_R | Perm_X);

    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    // .rodata
    rc = svcSetProcessMemoryPermission(
        g_procHandle, (u64)map_addr + header->segments[1].file_off, header->segments[1].size, Perm_R);

    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    // .data + .bss
    rc = svcSetProcessMemoryPermission(
        g_procHandle, (u64)map_addr + header->segments[2].file_off, rw_size, Perm_Rw);

    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    const u64 nro_size = header->segments[2].file_off + rw_size;
    const u64 nro_heap_start = ((u64) g_heapAddr) + nro_size;
    const u64 nro_heap_size  = g_heapSize + (u64) g_heapAddr - (u64) nro_heap_start;

    #define M EntryFlag_IsMandatory

    static ConfigEntry entries[] = {
        { EntryType_MainThreadHandle,     0, {0, 0} },
        { EntryType_ProcessHandle,        0, {0, 0} },
        { EntryType_AppletType,           0, {AppletType_SystemApplication, EnvAppletFlags_ApplicationOverride} },
        { EntryType_OverrideHeap,         M, {0, 0} },
        { EntryType_Argv,                 0, {0, 0} },
        { EntryType_NextLoadPath,         0, {0, 0} },
        { EntryType_LastLoadResult,       0, {0, 0} },
        { EntryType_SyscallAvailableHint, 0, {UINT64_MAX, UINT64_MAX} },
        { EntryType_SyscallAvailableHint2, 0, {UINT64_MAX, 0} },
        { EntryType_RandomSeed,           0, {0, 0} },
        { EntryType_UserIdStorage,        0, {(u64)(uintptr_t)&g_userIdStorage, 0} },
        { EntryType_HosVersion,           0, {0, 0} },
        { EntryType_EndOfList,            0, {(u64)(uintptr_t)g_noticeText, sizeof(g_noticeText)} }
    };

    ConfigEntry *entry_Syscalls = &entries[7];

    if (!(g_codeMemoryCapability & BIT(0))) {
        // Revoke access to svcCreateCodeMemory if it's not available.
        entry_Syscalls->Value[0x4B/64] &= ~(1UL << (0x4B%64));
    }

    if (!(g_codeMemoryCapability & BIT(1))) {
        // Revoke access to svcControlCodeMemory if it's not available for same-process usage.
        entry_Syscalls->Value[0x4C/64] &= ~(1UL << (0x4C%64)); // svcControlCodeMemory
    }

    // MainThreadHandle
    entries[0].Value[0] = envGetMainThreadHandle();
    // ProcessHandle
    entries[1].Value[0] = g_procHandle;
    // OverrideHeap
    entries[3].Value[0] = nro_heap_start;
    entries[3].Value[1] = nro_heap_size;
    // Argv
    entries[4].Value[1] = (u64)(uintptr_t)&g_argv[0];
    // NextLoadPath
    entries[5].Value[0] = (u64)(uintptr_t)&g_nextNroPath[0];
    entries[5].Value[1] = (u64)(uintptr_t)&g_nextArgv[0];
    // LastLoadResult
    entries[6].Value[0] = g_lastRet;
    // RandomSeed
    entries[9].Value[0] = randomGet64();
    entries[9].Value[1] = randomGet64();
    // HosVersion
    entries[11].Value[0] = hosversionGet();
    entries[11].Value[1] = hosversionIsAtmosphere() ? 0x41544d4f53504852UL : 0; // 'ATMOSPHR'

    g_nroAddr = (u64)map_addr;
    g_nroSize = nro_size;

    svcBreak(BreakReason_NotificationOnlyFlag | BreakReason_PostLoadDll, g_nroAddr, nro_size);

    // write exit detection
    strcpy(g_nextArgv, EXIT_DETECTION_STR);

    if (R_FAILED(rc = restoreMainThreadAffinity()))
        diagAbortWithResult(rc);

    // jump to trampoline.s
    nroEntrypointTrampoline(&entries[0], -1, g_nroAddr);
}

int main(int argc, char **argv) {
    memcpy(g_savedTls, (const u8*)armGetTls() + 0x100, 0x100);
    setupHbHeap();
    getOwnProcessHandle();
    getCodeMemoryCapability();
    loadNro();
}

// libnx stuff
u32 __nx_applet_type = AppletType_Application;
// Minimize fs resource usage
u32 __nx_fs_num_sessions = 1;
// these aren't needed, keeping them as someone will eventually
// copy this code, use fsdev, and not add back these vars.
u32 __nx_fsdev_direntry_cache_size = 1;
bool __nx_fsdev_support_cwd = false;
// enable to always exit to homemenu, dbi does this.
// u32 __nx_applet_exit_mode = 1;
// u32 __nx_applet_exit_mode = 0;

void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    fake_heap_start = NULL;
    fake_heap_end   = NULL;
}

void __appInit(void) {
    Result rc;

    // Detect Atmosphère early on. This is required for hosversion logic.
    // In the future, this check will be replaced by detectMesosphere().
    Handle dummy;
    rc = svcConnectToNamedPort(&dummy, "ams");
    u32 ams_flag = (R_VALUE(rc) != KERNELRESULT(NotFound)) ? BIT(31) : 0;
    if (R_SUCCEEDED(rc))
        svcCloseHandle(dummy);

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(ams_flag | MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, LibnxError_InitFail_FS));

    smExit(); // Close SM as we don't need it anymore.
}

void __appExit(void) {

}

// exit() effectively never gets called, so let's stub it out.
void __wrap_exit(void) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 39));
}

// stub alloc calls as they're not used (saves 4KiB).
void* __libnx_alloc(size_t size) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 40));
}

void* __libnx_aligned_alloc(size_t alignment, size_t size) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 41));
}

void __libnx_free(void* p) {
    diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 43));
}
