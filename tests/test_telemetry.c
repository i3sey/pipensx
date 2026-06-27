#include "../src/core/util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_contains(const char *path, const char *needle) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *data = (char*)calloc((size_t)size + 1, 1);
    assert(data);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    int found = strstr(data, needle) != NULL;
    free(data);
    return found;
}

int main(void) {
    const char *path = "/tmp/pipensx-telemetry-test.log";
    char backup[256];
    snprintf(backup, sizeof(backup), "%s.1", path);
    unlink(path);
    unlink(backup);

    log_init(path);
    uint32_t before = telemetry_generation();
    telemetry_set_enabled(1);
    assert(telemetry_enabled());
    assert(telemetry_generation() == before + 1);
    telemetry_log("test", "unit", "value=%d", 42);
    telemetry_set_enabled(0);
    telemetry_log("test", "unit", "suppressed=1");
    log_close();
    assert(file_contains(path, "stage=test tag=unit value=42"));
    assert(!file_contains(path, "suppressed=1"));

    FILE *large = fopen(path, "wb");
    assert(large);
    assert(fseek(large, 32L * 1024L * 1024L - 1, SEEK_SET) == 0);
    assert(fputc('x', large) == 'x');
    fclose(large);
    log_init(path);
    log_close();
    struct stat st;
    assert(stat(backup, &st) == 0);
    assert(st.st_size == 32L * 1024L * 1024L);

    unlink(path);
    unlink(backup);
    puts("telemetry tests passed");
    return 0;
}
