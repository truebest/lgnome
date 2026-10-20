#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *blocked_dir;
FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) {
    if (blocked_dir && strstr(path, ".write-test-") &&
        strncmp(path, blocked_dir, strlen(blocked_dir)) == 0 && mode[0] == 'w') {
        errno = EACCES;
        return NULL;
    }
    return __real_fopen(path, mode);
}

static unsigned sync_calls;
static unsigned fail_sync_call;
static int test_fsync(int fd);
#define fsync test_fsync
#include "../src/native_config_storage.c"
#undef fsync

static char pref_path[1024];

char *SDL_GetPrefPath(const char *org, const char *app) {
    (void)org;
    (void)app;
    return strdup(pref_path);
}

void SDL_free(void *ptr) { free(ptr); }
const char *SDL_GetError(void) { return "fake pref path"; }

static int test_fsync(int fd) {
    sync_calls++;
    if (sync_calls == fail_sync_call) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
    assert(chmod(path, 0600) == 0);
}

static void make_path(char *out, size_t cap, const char *dir, const char *name) {
    assert(native_config_join_path(out, cap, dir, name));
}

int main(void) {
    char root[] = "/tmp/lgnome-storage-test-XXXXXX";
    assert(mkdtemp(root));
    char primary_dir[1024], alternate_dir[1024], home[1024], high[1024], low[1024];
    make_path(primary_dir, sizeof(primary_dir), root, "primary");
    make_path(alternate_dir, sizeof(alternate_dir), root, "alternate");
    make_path(pref_path, sizeof(pref_path), root, "sdl");
    make_path(home, sizeof(home), root, "home");
    assert(mkdir(primary_dir, 0700) == 0);
    assert(mkdir(alternate_dir, 0700) == 0);
    assert(mkdir(pref_path, 0700) == 0);
    assert(mkdir(home, 0700) == 0);
    make_path(high, sizeof(high), primary_dir, "settings.json");
    make_path(low, sizeof(low), alternate_dir, "settings.json");
    char appid[80];
    snprintf(appid, sizeof(appid), "lgnome-storage-test-%lu", (unsigned long)getpid());
    assert(setenv("APPID", appid, 1) == 0);
    assert(setenv("HOME", home, 1) == 0);
    assert(setenv("LGNOME_NATIVE_SETTINGS_PATH", high, 1) == 0);
    assert(setenv("LGNOME_NATIVE_SETTINGS_DIR", alternate_dir, 1) == 0);
    unsetenv("LGNOME_IGNORE_SAVED_CONFIG");

    NativeConfigPathCandidates paths;
    native_config_collect_persisted_candidates(&paths);
    assert(paths.count == 11);
    assert(strncmp(paths.paths[10], "/tmp/", 5) == 0);

    NativeSettings s;
    native_settings_defaults(&s);
    write_text(high, "{\"sessions\":[]");
    write_text(low, "{\"wheelStep\":42}");
    assert(native_config_load_persisted(&s, false));
    assert(s.wheel_step == 42);
    assert(unlink(high) == 0);
    s.wheel_step = 43;
    assert(native_config_save_persisted(&s));
    assert(access(high, F_OK) == 0);
    assert(native_config_load_persisted(&s, false));
    assert(s.wheel_step == 43);
    write_text(high, "{\"sessions\":[]");

    FILE *embedded = fopen(high, "wb");
    static const char corrupt[] = "{\"wheelStep\":30}\0trailing";
    assert(embedded && fwrite(corrupt, 1, sizeof(corrupt) - 1, embedded) == sizeof(corrupt) - 1);
    assert(fclose(embedded) == 0);
    assert(native_config_load_persisted(&s, false));
    assert(s.wheel_step == 42);

    write_text(high, "{\"wheelStep\":34}");
    assert(chmod(primary_dir, 0500) == 0);
    blocked_dir = primary_dir;
    s.wheel_step = 35;
    assert(!native_config_save_persisted(&s));
    assert(native_config_load_persisted(&s, false));
    assert(s.wheel_step == 34);
    blocked_dir = NULL;
    assert(chmod(primary_dir, 0700) == 0);

    unsetenv("LGNOME_NATIVE_SETTINGS_PATH");
    unsetenv("LGNOME_NATIVE_SETTINGS_DIR");
    char discovered[1024], private_home[1024], home_settings[1024], uid_name[32], home_file[1024];
    make_path(discovered, sizeof(discovered), pref_path, "settings.json");
    make_path(home_settings, sizeof(home_settings), home, "settings");
    snprintf(uid_name, sizeof(uid_name), "%lu", (unsigned long)geteuid());
    make_path(private_home, sizeof(private_home), home_settings, uid_name);
    assert(native_config_mkdir_p(private_home));
    make_path(home_file, sizeof(home_file), private_home, "settings.json");
    write_text(home_file, "{\"wheelStep\":46}");
    write_text(high, "{\"wheelStep\":55}");
    assert(symlink(high, discovered) == 0);
    assert(chmod(pref_path, 0777) == 0);
    assert(native_config_load_persisted(&s, false));
    assert(s.wheel_step == 46);
    struct stat st;
    assert(stat(pref_path, &st) == 0 && (st.st_mode & 0777) == 0700);
    assert(unlink(discovered) == 0);
    write_text(discovered, "{\"wheelStep\":57}");
    const mode_t safe_modes[] = {0600, 0644, 0755};
    for (size_t i = 0; i < sizeof(safe_modes) / sizeof(safe_modes[0]); i++) {
        assert(chmod(pref_path, 0775) == 0);
        assert(chmod(discovered, safe_modes[i]) == 0);
        native_settings_defaults(&s);
        assert(native_config_load_persisted(&s, false));
        assert(s.wheel_step == 57);
        assert(stat(pref_path, &st) == 0 && (st.st_mode & 0777) == 0700);
        assert(stat(discovered, &st) == 0 && (st.st_mode & 0777) == safe_modes[i]);
    }
    const mode_t writable_modes[] = {0620, 0602, 0664, 0666, 0775};
    for (size_t i = 0; i < sizeof(writable_modes) / sizeof(writable_modes[0]); i++) {
        assert(chmod(pref_path, 0775) == 0);
        assert(chmod(discovered, writable_modes[i]) == 0);
        native_settings_defaults(&s);
        assert(native_config_load_persisted(&s, false));
        assert(s.wheel_step == 46);
        assert(stat(pref_path, &st) == 0 && (st.st_mode & 0777) == 0700);
        assert(stat(discovered, &st) == 0 && (st.st_mode & 0777) == writable_modes[i]);
    }
    assert(unlink(discovered) == 0);

    s.wheel_step = 47;
    assert(native_config_save_persisted(&s));
    assert(unlink(home_file) == 0);
    assert(rmdir(private_home) == 0);
    assert(symlink(primary_dir, private_home) == 0);
    s.wheel_step = 48;
    assert(native_config_save_persisted(&s));
    NativeSettings untouched;
    native_settings_defaults(&untouched);
    assert(native_config_load_file(&untouched, high, true));
    assert(untouched.wheel_step == 55);
    assert(native_config_load_persisted(&s, false));
    assert(s.wheel_step == 48);

    sync_calls = 0;
    fail_sync_call = 1;
    s.wheel_step = 49;
    assert(!native_settings_save_file(&s, high));
    assert(native_config_load_file(&untouched, high, true));
    assert(untouched.wheel_step == 55);
    fail_sync_call = 0;
    sync_calls = 0;
    assert(native_settings_save_file(&s, high));
    assert(sync_calls == 2);
    sync_calls = 0;
    fail_sync_call = 2;
    s.wheel_step = 50;
    assert(!native_settings_save_file(&s, high));
    assert(native_config_load_file(&untouched, high, true));
    assert(untouched.wheel_step == 50);
    fail_sync_call = 0;

    assert(unlink(discovered) == 0);
    assert(unlink(private_home) == 0);
    assert(rmdir(home_settings) == 0);
    assert(rmdir(home) == 0);
    assert(unlink(high) == 0);
    assert(unlink(low) == 0);
    assert(rmdir(primary_dir) == 0);
    assert(rmdir(alternate_dir) == 0);
    assert(rmdir(pref_path) == 0);
    assert(rmdir(root) == 0);
    puts("config-storage: all checks passed");
    return 0;
}
