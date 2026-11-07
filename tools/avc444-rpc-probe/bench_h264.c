#define _POSIX_C_SOURCE 200809L
#include "h264_annexb.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END)) return 2;
    long size = ftell(file);
    if (size <= 0 || size > (long)NATIVE_H264_MAX_AU_BYTES || fseek(file, 0, SEEK_SET)) return 2;
    unsigned char *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) return 2;
    fclose(file);
    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return 2;
    unsigned total = 0;
    for (unsigned i = 0; i < 16; ++i) {
        NativeH264Info info;
        if (native_h264_scan_annexb(data, (size_t)size, &info) != NATIVE_H264_OK) return 1;
        total += info.nal_count;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end)) return 2;
    double seconds = (double)(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("bytes=%ld passes=16 NALs=%u seconds=%.6f MB_per_s=%.3f\n",
        size, total, seconds, size * 16.0 / seconds / 1e6);
    free(data);
    return 0;
}
