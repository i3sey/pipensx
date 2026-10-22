#include <switch.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u64 programId = 0;
    svcGetInfo(&programId, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);

    char iniPath[FS_MAX_PATH];
    snprintf(iniPath, sizeof(iniPath),
             "sdmc:/switch/wine/forwarders/%016llX.ini",
             (unsigned long long)programId);

    char nroPath[FS_MAX_PATH] = "sdmc:/switch/wine/wine-nx-runtime.nro";
    char nroArgv[FS_MAX_PATH * 2] = {0};

    FILE* f = fopen(iniPath, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char* key = line;
            char* val = eq + 1;
            while (*key == ' ' || *key == '\t') key++;
            size_t klen = strlen(key);
            while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t')) {
                key[--klen] = '\0';
            }
            while (*val == ' ' || *val == '\t') val++;
            size_t vlen = strlen(val);
            while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' ||
                                val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
                val[--vlen] = '\0';
            }
            if (strcmp(key, "nro") == 0) {
                snprintf(nroPath, sizeof(nroPath), "%s", val);
            } else if (strcmp(key, "argv") == 0) {
                snprintf(nroArgv, sizeof(nroArgv), "%s", val);
            }
        }
        fclose(f);
    }

    if (nroArgv[0] == '\0') {
        snprintf(nroArgv, sizeof(nroArgv), "\"%s\"", nroPath);
    }

    envSetNextLoad(nroPath, nroArgv);
    return 0;
}
