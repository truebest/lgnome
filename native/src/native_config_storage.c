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

#ifdef LGNOME_TARGET_WEBOS
#include <SDL.h>
#endif

#include "config_paths.h"
#include "native_config.h"
#include "settings_json.h"

#include "clog.h"

clog_define(g_native_log_config_storage, cLogLevelInfo, "native");

#ifdef LGNOME_TARGET_WEBOS
static const char native_config_app_id[] = "com.truebest.lgnome.native";
#endif

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
         (st.st_mode & (S_IWGRP | S_IWOTH)))) {
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
    fclose(file);
    if (read_count != (size_t)size || memchr(json, '\0', read_count)) {
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

static bool native_settings_save_file_checked(const NativeSettings *settings, const char *path,
                                              bool private_dir) {
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
    bool ok = native_settings_write_json(settings, file);
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

bool native_settings_save_file(const NativeSettings *settings, const char *path) {
    return native_settings_save_file_checked(settings, path, false);
}

#ifdef LGNOME_TARGET_WEBOS
static void native_config_collect_persisted_candidates(NativeConfigPathCandidates *candidates) {
    memset(candidates, 0, sizeof(*candidates));

    /* User-provided overrides are trusted (from_env=true): they are honored for reads in
     * priority order even on a read-only or foreign-owned mount. Discovered locations below
     * must pass the ownership/permission check before their contents are written or trusted. */
    (void)native_config_add_candidate_path(candidates, getenv("LGNOME_NATIVE_SETTINGS_PATH"), true);
    (void)native_config_add_candidate_dir(candidates, getenv("LGNOME_NATIVE_SETTINGS_DIR"), true);

    char *pref_path = SDL_GetPrefPath("truebest", "lgnome");
    if (pref_path) {
        (void)native_config_add_candidate_dir(candidates, pref_path, false);
        SDL_free(pref_path);
    } else {
        clog(cLogLevelWarning, "failed to resolve SDL pref path: %s", SDL_GetError());
    }

    const char *appid = getenv("APPID");
    if (!appid || !appid[0]) {
        appid = native_config_app_id;
    }

    /* Preferred location (user choice): the IPK ships <approot>/settings with mode
     * 01777 (tools/build-native-webos.sh) because the install tree is root-owned on the
     * TV; the app-private euid-suffixed subdir inside follows the exact trust model of
     * the /tmp fallback -- but on persistent storage. The dev-mode jail sets HOME to the
     * app root itself (verified live; SDL_GetPrefPath is what points at bin/). */
    const char *home = getenv("HOME");
    if (home && home[0] && strcmp(home, "/") != 0) {
        char inapp_dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
        int an = snprintf(inapp_dir, sizeof(inapp_dir), "%s/settings/%lu", home, (unsigned long)geteuid());
        if (an > 0 && (size_t)an < sizeof(inapp_dir)) {
            (void)native_config_add_candidate_dir(candidates, inapp_dir, false);
        }
    }

    (void)native_config_add_candidate_app_dir(candidates, "/var/luna/preferences/%s", appid);
    (void)native_config_add_candidate_app_dir(candidates, "/media/developer/apps/usr/palm/data/%s", appid);
    /* Dev-mode app data commonly lives on cryptofs (persistent ext4). */
    (void)native_config_add_candidate_app_dir(candidates, "/media/cryptofs/apps/usr/palm/data/%s", appid);
    (void)native_config_add_candidate_app_dir(candidates, "/media/internal/%s", appid);

    /* /media/developer is the ONE persistent ext4 the dev-mode jail maps read-write
     * (verified live via /proc/self/mounts -- every conventional data dir above is
     * root-owned or a read-only mount for this app's uid). Its temp/ is world-writable
     * like /tmp, so an app-private euid-suffixed subdir passes the exact ownership
     * checks the /tmp fallback below relies on -- but it survives TV power cycles, which
     * tmpfs does not (observed live: settings vanished on the first cold boot). */
    char devtemp_dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    int dn = snprintf(devtemp_dir, sizeof(devtemp_dir), "/media/developer/temp/%s-%lu", appid,
                      (unsigned long)geteuid());
    if (dn > 0 && (size_t)dn < sizeof(devtemp_dir)) {
        (void)native_config_add_candidate_dir(candidates, devtemp_dir, false);
    }

    (void)native_config_add_candidate_app_dir(candidates, "/var/run/%s", appid);

    char fallback_dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
    int n = snprintf(fallback_dir, sizeof(fallback_dir), "/tmp/%s-%lu", appid, (unsigned long)geteuid());
    if (n > 0 && (size_t)n < sizeof(fallback_dir)) {
        (void)native_config_add_candidate_dir(candidates, fallback_dir, false);
    }
}

typedef enum NativeConfigLoadOutcome {
    NATIVE_CONFIG_LOAD_MISSING, /* no file at this path */
    NATIVE_CONFIG_LOAD_INVALID, /* file exists but could not be read or parsed */
    NATIVE_CONFIG_LOAD_OK,      /* file parsed and applied into *config */
} NativeConfigLoadOutcome;

/* Load one candidate with a single open, distinguishing "no file here" from "file is
 * corrupt" so the caller can fall through to lower-priority candidates in either case. */
static NativeConfigLoadOutcome native_config_try_load_candidate(NativeSettings *settings, const char *path,
                                                                bool private_file) {
    char *json = NULL;
    bool existed = false;
    if (!native_config_read_file(path, &json, &existed, private_file)) {
        return existed ? NATIVE_CONFIG_LOAD_INVALID : NATIVE_CONFIG_LOAD_MISSING;
    }
    if (!existed) {
        return NATIVE_CONFIG_LOAD_MISSING;
    }

    bool ok = native_settings_apply_json(settings, json, path);
    free(json);
    if (!ok) {
        return NATIVE_CONFIG_LOAD_INVALID;
    }
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
    native_config_collect_persisted_candidates(&candidates);

    /* Load from the highest-priority candidate whose file exists and parses. Writability plays
     * no part in load ordering (an env override on a read-only mount must still win over a
     * stale writable copy), and a corrupt higher-priority file must not hide a valid
     * lower-priority one. Discovered (non-env) directories must be owned by us and not
     * other-writable before their contents are trusted, so a local attacker cannot plant a
     * settings.json (pointing at their own RDP host) for us to load. */
    for (size_t i = 0; i < candidates.count; i++) {
        if (!candidates.from_env[i]) {
            char dir[NATIVE_PERSISTED_CONFIG_PATH_MAX];
            if (!native_config_parent_dir(candidates.paths[i], dir, sizeof(dir))) {
                continue;
            }
            if (!native_config_dir_secure_or_heal(dir)) {
                /* Never silent: a foreign-owned directory here made saved settings
                 * vanish without a trace in the log once already. */
                struct stat st;
                if (stat(dir, &st) == 0) {
                    clog(cLogLevelWarning,
                         "persisted-config candidate skipped (dir not private): %s uid=%lu mode=%lo", dir,
                         (unsigned long)st.st_uid, (unsigned long)(st.st_mode & 07777));
                }
                continue;
            }
        }

        NativeConfigLoadOutcome outcome = native_config_try_load_candidate(
            settings, candidates.paths[i], !candidates.from_env[i]);
        if (outcome == NATIVE_CONFIG_LOAD_MISSING) {
            continue;
        }
        if (outcome == NATIVE_CONFIG_LOAD_INVALID) {
            clog(cLogLevelWarning, "ignored invalid persisted config: %s", candidates.paths[i]);
            continue;
        }
        clog(cLogLevelInfo, "loaded persisted config: %s", candidates.paths[i]);
        return true;
    }
    return true;
}

bool native_config_save_persisted(const NativeSettings *settings) {
    NativeConfigPathCandidates candidates;
    native_config_collect_persisted_candidates(&candidates);
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
                                             !candidates.from_env[i]) == NATIVE_CONFIG_LOAD_OK) {
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
