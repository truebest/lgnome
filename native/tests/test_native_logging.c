#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "native_lifecycle.h"
#include "clog.h"

clog_define(g_test_log, cLogLevelInfo, "test.logging");
void native_media_emergency_release(void) {}

int main(void) {
    char root[] = "/tmp/lgnome-logging-test-XXXXXX";
    assert(mkdtemp(root));
    char path[512];
    snprintf(path, sizeof(path), "%s/missing/log", root);
    int original = dup(STDERR_FILENO);
    assert(original >= 0);
    assert(setenv("LGNOME_NATIVE_LOG_PATH", path, 1) == 0);
    native_prepare_webos_logging();
    assert(fcntl(STDERR_FILENO, F_GETFD) >= 0);

    snprintf(path, sizeof(path), "%s/log", root);
    assert(setenv("LGNOME_NATIVE_LOG_PATH", path, 1) == 0);
    native_prepare_webos_logging();
    clog(cLogLevelError, "logging-regression-marker");
    assert(fflush(stderr) == 0);
    assert(dup2(original, STDERR_FILENO) >= 0);
    close(original);
    FILE *file = fopen(path, "rb");
    assert(file);
    char buffer[512] = {0};
    assert(fread(buffer, 1, sizeof(buffer) - 1, file) > 0);
    fclose(file);
    assert(strstr(buffer, "logging-regression-marker"));
    assert(unlink(path) == 0);
    assert(rmdir(root) == 0);
    puts("native-logging: all checks passed");
    return 0;
}
