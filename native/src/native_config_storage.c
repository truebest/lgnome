#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "native_config_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config_paths.h"
#include "native_config.h"
#include "settings_json.h"

#include "clog.h"

clog_define(g_native_log_config_storage, cLogLevelInfo, "native");

/* Reads the whole file at `path` into a NUL-terminated heap buffer with a single open.
 *   missing file (open fails):            *existed=false, *out=NULL, returns true
 *   read/size error on an existing file:  *existed=true,  *out=NULL, returns false
 *   success:                              *existed=true,  *out=buffer (caller frees), returns true */
static bool native_config_read_file(const char *path, char **out, bool *existed, bool private_file) {
    *out = NULL;
    *existed = false;

    int fd = open(path, O_RDONLY | (private_file ? O_NOFOLLOW | O_NONBLOCK : 0));
    if (fd < 0) {
        if (errno != ENOENT && errno != ENOTDIR) {
            *existed = true;
            return false;
        }
        return true;
    }
    *existed = true;
    struct stat st;
    if (private_file &&
        (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
         st.st_nlink != 1)) {
        close(fd);
        return false;
    }
    if (private_file && (st.st_mode & 0777) != 0600 &&
        (fchmod(fd, 0600) != 0 || fstat(fd, &st) != 0)) {
        close(fd);
        return false;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        clog(cLogLevelError, "failed to inspect config file: %s", path);
        return false;
    }
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > NATIVE_CONFIG_MAX_FILE) {
        fclose(file);
        clog(cLogLevelError, "config file is too large: %s", path);
        return false;
    }
    rewind(file);

    char *json = (char *)calloc((size_t)size + 5, 1);
    if (!json) {
        fclose(file);
        clog(cLogLevelError, "failed to allocate config buffer");
        return false;
    }

    size_t read_count = fread(json, 1, (size_t)size, file);
    struct stat after;
    bool changed = private_file &&
                   (fstat(fd, &after) != 0 || st.st_size != after.st_size ||
                    st.st_mtim.tv_sec != after.st_mtim.tv_sec ||
                    st.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
                    st.st_ctim.tv_sec != after.st_ctim.tv_sec ||
                    st.st_ctim.tv_nsec != after.st_ctim.tv_nsec);
    fclose(file);
    if (changed || read_count != (size_t)size || memchr(json, '\0', read_count)) {
        free(json);
        clog(cLogLevelError, "failed to read config file: %s", path);
        return false;
    }

    *out = json;
    return true;
}

static bool native_config_load_file_internal(NativeSettings *settings, const char *path, bool required,
                                             bool log_missing) {
    char *json = NULL;
    bool existed = false;
    if (!native_config_read_file(path, &json, &existed, false)) {
        return false;
    }
    if (!existed) {
        if (required) {
            clog(cLogLevelError, "config file not found: %s", path);
            return false;
        }
        if (log_missing) {
            clog(cLogLevelInfo, "config file not found at %s; using defaults and CLI overrides", path);
        }
        return true;
    }

    bool ok = native_settings_apply_json(settings, json, path);
    free(json);
    if (!ok) {
        clog(cLogLevelError, "failed to parse config file: %s", path);
        return false;
    }
    clog(cLogLevelInfo, "loaded config file: %s", path);
    return true;
}

bool native_config_load_file(NativeSettings *settings, const char *path, bool required) {
    return native_config_load_file_internal(settings, path, required, true);
}

static bool native_config_write_file_checked(const NativeSettings *settings, const char *json,
                                             const char *path, bool private_dir) {
    char dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    if (!native_config_parent_dir(path, dir, sizeof(dir))) return false;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char temporary[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp", name);
    if (n <= 0 || (size_t)n >= sizeof(temporary) || !name[0]) return false;

    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0) return false;
    struct stat st;
    if (private_dir &&
        (fstat(dir_fd, &st) != 0 || st.st_uid != geteuid() ||
         (st.st_mode & (S_IWGRP | S_IWOTH)))) {
        close(dir_fd);
        return false;
    }
    /* Pin the verified directory across creation and rename. */
    (void)unlinkat(dir_fd, temporary, 0);
    int fd = openat(dir_fd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        close(dir_fd);
        return false;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        (void)unlinkat(dir_fd, temporary, 0);
        close(dir_fd);
        return false;
    }
    bool ok = settings ? native_settings_write_json(settings, file) : fputs(json, file) >= 0;
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > NATIVE_CONFIG_MAX_FILE) ok = false;
    if (ok && (fflush(file) != 0 || fsync(fd) != 0)) ok = false;
    if (fclose(file) != 0) ok = false;
    if (ok && renameat(dir_fd, temporary, dir_fd, name) != 0) ok = false;
    if (ok && fsync(dir_fd) != 0) ok = false;
    if (!ok) {
        (void)unlinkat(dir_fd, temporary, 0);
        clog(cLogLevelError, "failed to persist config at %s", path);
    } else {
        clog(cLogLevelInfo, "saved persisted config: %s", path);
    }
    close(dir_fd);
    return ok;
}

static bool native_settings_save_file_checked(const NativeSettings *settings, const char *path,
                                              bool private_dir) {
    return native_config_write_file_checked(settings, NULL, path, private_dir);
}

bool native_settings_save_file(const NativeSettings *settings, const char *path) {
    return native_settings_save_file_checked(settings, path, false);
}

#ifdef LGNOME_TARGET_WEBOS
static bool native_config_collect_persisted_candidates(NativeConfigPathCandidates *candidates) {
    memset(candidates, 0, sizeof(*candidates));
    (void)native_config_add_candidate_path(candidates, getenv("LGNOME_NATIVE_SETTINGS_PATH"), true);
    (void)native_config_add_candidate_dir(candidates, getenv("LGNOME_NATIVE_SETTINGS_DIR"), true);

    const char *home = getenv("HOME");
    if (home && home[0] && strcmp(home, "/") != 0) {
        char path[NATIVE_PERSISTED_CONFIG_PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/settings/%lu/%s", home, (unsigned long)geteuid(),
                         NATIVE_PERSISTED_CONFIG_FILENAME);
        if (n > 0 && (size_t)n < sizeof(path)) {
            (void)native_config_add_candidate_path(candidates, path, false);
        }
    }
    if (candidates->count == 0) {
        clog(cLogLevelError, "persistent settings path is unavailable");
        return false;
    }
    return true;
}

typedef enum NativeConfigLoadOutcome {
    NATIVE_CONFIG_LOAD_MISSING, /* no file at this path */
    NATIVE_CONFIG_LOAD_INVALID, /* file exists but could not be read or parsed */
    NATIVE_CONFIG_LOAD_OK,      /* file parsed and applied into *config */
} NativeConfigLoadOutcome;

/* Load one candidate with a single open, distinguishing "no file here" from "file is
 * corrupt" so the caller can fall through to lower-priority candidates in either case. */
static NativeConfigLoadOutcome native_config_try_load_candidate(NativeSettings *settings, const char *path,
                                                                bool private_file, char **validated_json) {
    if (validated_json) *validated_json = NULL;
    char *json = NULL;
    bool existed = false;
    if (!native_config_read_file(path, &json, &existed, private_file)) {
        return existed ? NATIVE_CONFIG_LOAD_INVALID : NATIVE_CONFIG_LOAD_MISSING;
    }
    if (!existed) {
        return NATIVE_CONFIG_LOAD_MISSING;
    }

    bool ok = native_settings_apply_json(settings, json, path);
    if (!ok) {
        free(json);
        return NATIVE_CONFIG_LOAD_INVALID;
    }
    if (validated_json) *validated_json = json;
    else free(json);
    return NATIVE_CONFIG_LOAD_OK;
}

bool native_config_load_persisted(NativeSettings *settings, bool force_ignore) {
    const char *ignore = getenv("LGNOME_IGNORE_SAVED_CONFIG");
    if (force_ignore || (ignore && strcmp(ignore, "1") == 0)) {
        clog(cLogLevelInfo,
             "skipped persisted config because saved settings were disabled for this launch");
        return true;
    }

    NativeConfigPathCandidates candidates;
    if (!native_config_collect_persisted_candidates(&candidates)) return false;

    for (size_t i = 0; i < candidates.count; i++) {
        if (!candidates.from_env[i]) {
            char dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
            if (!native_config_parent_dir(candidates.paths[i], dir, sizeof(dir))) return false;
            if (!native_config_dir_secure_or_heal(dir)) {
                struct stat st;
                if (lstat(dir, &st) != 0 && errno == ENOENT) continue;
                clog(cLogLevelError, "persistent settings directory is unavailable: %s", dir);
                return false;
            }
        }

        NativeSettings loaded = *settings;
        char *json = NULL;
        NativeConfigLoadOutcome outcome = native_config_try_load_candidate(
            &loaded, candidates.paths[i], !candidates.from_env[i], &json);
        if (outcome == NATIVE_CONFIG_LOAD_MISSING) continue;
        if (outcome == NATIVE_CONFIG_LOAD_INVALID) {
            clog(cLogLevelWarning, "invalid persisted config: %s", candidates.paths[i]);
            if (!candidates.from_env[i]) return false;
            continue;
        }
        /* Replace the inode so an earlier writable descriptor cannot alter future loads. */
        bool protected = candidates.from_env[i] ||
                         native_config_write_file_checked(NULL, json, candidates.paths[i], true);
        free(json);
        if (!protected) {
            clog(cLogLevelError, "failed to protect persisted config: %s", candidates.paths[i]);
            return false;
        }
        *settings = loaded;
        clog(cLogLevelInfo, "loaded persisted config: %s", candidates.paths[i]);
        return true;
    }
    return true;
}

bool native_config_save_persisted(const NativeSettings *settings) {
    NativeConfigPathCandidates candidates;
    if (!native_config_collect_persisted_candidates(&candidates)) return false;
    size_t save_index;
    if (!native_config_find_persisted_save_candidate(&candidates, &save_index)) {
        clog(cLogLevelWarning, "no writable persisted config directory found");
        return false;
    }
    for (size_t i = 0; i < candidates.count; i++) {
        if (!candidates.from_env[i]) {
            char dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
            if (!native_config_parent_dir(candidates.paths[i], dir, sizeof(dir)) ||
                !native_config_dir_secure_or_heal(dir)) {
                continue;
            }
        }
        NativeSettings existing = *settings;
        if (native_config_try_load_candidate(&existing, candidates.paths[i],
                                             !candidates.from_env[i], NULL) == NATIVE_CONFIG_LOAD_OK) {
            if (save_index > i) {
                clog(cLogLevelError, "loaded settings location is not writable");
                return false;
            }
            break;
        }
    }
    clog(cLogLevelInfo, "using persisted config path: %s", candidates.paths[save_index]);
    return native_settings_save_file_checked(settings, candidates.paths[save_index], true);
}
#else
bool native_config_load_persisted(NativeSettings *settings, bool force_ignore) {
    (void)settings;
    (void)force_ignore;
    return true;
}

bool native_config_save_persisted(const NativeSettings *settings) {
    (void)settings;
    return true;
}
#endif
