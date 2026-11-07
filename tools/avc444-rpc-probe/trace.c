#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "clog.h"

clog_define(rpc_probe_log, cLogLevelInfo, "avc444.rpc-probe");

static const char *names[] = {
    "vrpc_write", "vrpc_inband_pts2", "vrpc_inband_decode",
    "vrpc_composer_enqueue", "vrpc_composer_dequeue", "DirectVoGetFrame",
    "DirectVoUpdateFrameInfo", "DirectVoUpdateWriteIndex", "DirectVoRelease",
    "vrpc_composer_connect_decoder", "vrpc_composer_connect_dvo"
};
static void *functions[sizeof(names) / sizeof(names[0])];
static unsigned counts[sizeof(names) / sizeof(names[0])];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int stream_fd = -1;
static size_t dumped;

__attribute__((constructor)) static void start(void) {
    int fd = open("/tmp/lgnome-rpc-trace.log", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) _exit(125);
    dup2(fd, STDERR_FILENO);
    close(fd);
    stream_fd = open("/tmp/lgnome-rpc-stream.bin", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (stream_fd < 0) _exit(125);
    clog(cLogLevelInfo, "passive RPC trace started");
}

/* ARM shims preserve the original argument registers, stack and return address. */
__attribute__((visibility("hidden"))) void *rpc_trace_enter(unsigned id, const uint32_t *args) {
    pthread_mutex_lock(&lock);
    if (!functions[id]) {
        functions[id] = dlsym(RTLD_NEXT, names[id]);
        if (!functions[id]) {
            void *lib = dlopen("librpccore.so", RTLD_LAZY | RTLD_NOLOAD);
            if (lib) { functions[id] = dlsym(lib, names[id]); dlclose(lib); }
        }
        if (!functions[id]) _exit(125);
    }
    unsigned count = ++counts[id];
    if (count <= 8) {
        clog(cLogLevelInfo, "%s call=%u args=%08x,%08x,%08x,%08x",
             names[id], count, args[0], args[1], args[2], args[3]);
    }
    if (id == 0 && args[2] && args[3] <= 4u * 1024u * 1024u && dumped < 32u * 1024u * 1024u) {
        uint32_t record[3] = {args[1], args[3], count};
        const uint8_t *data = (const uint8_t *)(uintptr_t)args[2];
        if (write(stream_fd, record, sizeof(record)) != sizeof(record) ||
            write(stream_fd, data, args[3]) != (ssize_t)args[3]) _exit(125);
        dumped += sizeof(record) + args[3];
    }
    void *result = functions[id];
    pthread_mutex_unlock(&lock);
    return result;
}

__attribute__((destructor)) static void finish(void) {
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        clog(cLogLevelInfo, "total %s=%u", names[i], counts[i]);
    }
    if (stream_fd >= 0) close(stream_fd);
}
