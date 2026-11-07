#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
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
static bool foreign_file_owner;
static int truncate_after_read_fd = -1;
static int test_fsync(int fd);
static int test_fstat(int fd, struct stat *st);
static size_t test_fread(void *buffer, size_t size, size_t count, FILE *file);
#define fsync test_fsync
#define fstat test_fstat
#define fread test_fread
#include "../src/native_config_storage.c"
#undef fsync
#undef fstat
#undef fread

static int test_fsync(int fd) {
    sync_calls++;
    if (sync_calls == fail_sync_call) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static int test_fstat(int fd, struct stat *st) {
    int result = fstat(fd, st);
    if (result == 0 && foreign_file_owner && S_ISREG(st->st_mode)) st->st_uid = geteuid() + 1;
    return result;
}

static size_t test_fread(void *buffer, size_t size, size_t count, FILE *file) {
    size_t result = fread(buffer, size, count, file);
    if (truncate_after_read_fd >= 0) assert(ftruncate(truncate_after_read_fd, 0) == 0);
    return result;
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
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
    make_path(home, sizeof(home), root, "home");
    assert(mkdir(primary_dir, 0700) == 0);
    assert(mkdir(alternate_dir, 0700) == 0);
    assert(mkdir(home, 0700) == 0);
    make_path(high, sizeof(high), primary_dir, "settings.json");
    make_path(low, sizeof(low), alternate_dir, "settings.json");
    assert(setenv("HOME", home, 1) == 0);
    assert(setenv("LGNOME_NATIVE_SETTINGS_PATH", high, 1) == 0);
    assert(setenv("LGNOME_NATIVE_SETTINGS_DIR", alternate_dir, 1) == 0);
    unsetenv("LGNOME_IGNORE_SAVED_CONFIG");

    char settings_dir[1024], private_home[1024], home_file[1024], uid_name[32];
    make_path(settings_dir, sizeof(settings_dir), home, "settings");
    snprintf(uid_name, sizeof(uid_name), "%lu", (unsigned long)geteuid());
    make_path(private_home, sizeof(private_home), settings_dir, uid_name);
    make_path(home_file, sizeof(home_file), private_home, "settings.json");
    NativeConfigPathCandidates paths;
    assert(native_config_collect_persisted_candidates(&paths));
    assert(paths.count == 3 && strcmp(paths.paths[2], home_file) == 0);

    NativeSettings s;
    native_settings_defaults(&s);
    write_text(high, "{\"sessions\":[]");
    write_text(low, "{\"wheelStep\":42}");
    assert(native_config_load_persisted(&s, false));
    assert(s.wheel_step == 42);
    assert(unlink(high) == 0);
    s.wheel_step = 43;
    assert(native_config_save_persisted(&s));
    assert(native_config_load_persisted(&s, false) && s.wheel_step == 43);

    FILE *embedded = fopen(high, "wb");
    static const char corrupt[] = "{\"wheelStep\":30}\0trailing";
    assert(embedded && fwrite(corrupt, 1, sizeof(corrupt) - 1, embedded) == sizeof(corrupt) - 1);
    assert(fclose(embedded) == 0);
    assert(native_config_load_persisted(&s, false) && s.wheel_step == 42);

    write_text(high, "{\"wheelStep\":34}");
    assert(chmod(primary_dir, 0500) == 0);
    blocked_dir = primary_dir;
    s.wheel_step = 35;
    assert(!native_config_save_persisted(&s));
    assert(native_config_load_persisted(&s, false) && s.wheel_step == 34);
    blocked_dir = NULL;
    assert(chmod(primary_dir, 0700) == 0);

    unsetenv("LGNOME_NATIVE_SETTINGS_PATH");
    unsetenv("LGNOME_NATIVE_SETTINGS_DIR");
    assert(native_config_collect_persisted_candidates(&paths));
    assert(paths.count == 1 && strcmp(paths.paths[0], home_file) == 0);
    assert(native_config_load_persisted(&s, false));
    s.wheel_step = 46;
    assert(native_config_save_persisted(&s));
    struct stat st;
    assert(stat(private_home, &st) == 0 && (st.st_mode & 0777) == 0700);
    assert(stat(home_file, &st) == 0 && (st.st_mode & 0777) == 0600);

    const mode_t drifted_modes[] = {0600, 0644, 0755, 0664, 0666, 0775, 0777};
    for (size_t i = 0; i < sizeof(drifted_modes) / sizeof(drifted_modes[0]); i++) {
        write_text(home_file, "{\"wheelStep\":57}");
        assert(chmod(private_home, 0775) == 0);
        assert(chmod(home_file, drifted_modes[i]) == 0);
        native_settings_defaults(&s);
        assert(native_config_load_persisted(&s, false) && s.wheel_step == 57);
        assert(stat(private_home, &st) == 0 && (st.st_mode & 0777) == 0700);
        assert(stat(home_file, &st) == 0 && (st.st_mode & 0777) == 0600);
    }

    /* Loading a partial file must not persist this launch's effective overrides. */
    char default_host[NATIVE_SETTINGS_STRING_MAX];
    snprintf(default_host, sizeof(default_host), "%s", s.sessions[NATIVE_SESSION_SLOT_GREEN].host);
    snprintf(s.sessions[NATIVE_SESSION_SLOT_GREEN].host, NATIVE_SETTINGS_STRING_MAX, "temporary.example");
    s.camera_enabled = true;
    assert(native_config_load_persisted(&s, false));
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].host, "temporary.example") == 0);
    assert(s.camera_enabled);
    char *sealed_json = NULL;
    bool existed = false;
    assert(native_config_read_file(home_file, &sealed_json, &existed, true) && existed);
    assert(strcmp(sealed_json, "{\"wheelStep\":57}") == 0);
    free(sealed_json);
    native_settings_defaults(&s);
    assert(native_config_load_persisted(&s, false) && s.wheel_step == 57);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].host, default_host) == 0);
    assert(!s.camera_enabled);

    /* A writer opened before permission repair must retain only the replaced inode. */
    write_text(home_file, "{\"wheelStep\":58}");
    int old_writer = open(home_file, O_RDWR);
    assert(old_writer >= 0 && fstat(old_writer, &st) == 0);
    ino_t old_inode = st.st_ino;
    assert(chmod(home_file, 0775) == 0);
    assert(native_config_load_persisted(&s, false) && s.wheel_step == 58);
    assert(stat(home_file, &st) == 0 && st.st_ino != old_inode);
    const char altered[] = "{\"wheelStep\":99}";
    assert(pwrite(old_writer, altered, sizeof(altered) - 1, 0) == (ssize_t)sizeof(altered) - 1);
    assert(close(old_writer) == 0);
    assert(native_config_load_persisted(&s, false) && s.wheel_step == 58);

    write_text(home_file, "{\"wheelStep\":59}");
    truncate_after_read_fd = open(home_file, O_RDWR);
    assert(truncate_after_read_fd >= 0);
    assert(!native_config_load_persisted(&s, false) && s.wheel_step == 58);
    assert(close(truncate_after_read_fd) == 0);
    truncate_after_read_fd = -1;
    assert(stat(home_file, &st) == 0 && st.st_size == 0);

    write_text(home_file, "{\"wheelStep\":60}");
    assert(chmod(home_file, 0775) == 0);
    foreign_file_owner = true;
    assert(!native_config_load_persisted(&s, false) && s.wheel_step == 58);
    foreign_file_owner = false;
    assert(stat(home_file, &st) == 0 && (st.st_mode & 0777) == 0775);
    assert(unlink(home_file) == 0);

    assert(symlink(high, home_file) == 0);
    assert(!native_config_load_persisted(&s, false));
    assert(unlink(home_file) == 0);
    assert(mkfifo(home_file, 0600) == 0);
    assert(!native_config_load_persisted(&s, false));
    assert(unlink(home_file) == 0);
    assert(link(high, home_file) == 0);
    assert(!native_config_load_persisted(&s, false));
    assert(unlink(home_file) == 0);

    write_text(home_file, "{\"sessions\":[]");
    assert(!native_config_load_persisted(&s, false) && s.wheel_step == 58);
    assert(stat(home_file, &st) == 0 && st.st_size == 14);
    write_text(home_file, "{\"wheelStep\":61}");
    assert(native_config_load_persisted(&s, false) && s.wheel_step == 61);

    sync_calls = 0;
    fail_sync_call = 1;
    s.wheel_step = 62;
    assert(!native_config_load_persisted(&s, false) && s.wheel_step == 62);
    fail_sync_call = 0;
    assert(native_config_load_persisted(&s, false) && s.wheel_step == 61);

    assert(setenv("LGNOME_NATIVE_SETTINGS_PATH", high, 1) == 0);
    NativeSettings untouched;
    native_settings_defaults(&untouched);
    sync_calls = 0;
    fail_sync_call = 1;
    s.wheel_step = 49;
    assert(!native_settings_save_file(&s, high));
    assert(native_config_load_file(&untouched, high, true) && untouched.wheel_step == 34);
    fail_sync_call = 0;
    sync_calls = 0;
    assert(native_settings_save_file(&s, high) && sync_calls == 2);
    sync_calls = 0;
    fail_sync_call = 2;
    s.wheel_step = 50;
    assert(!native_settings_save_file(&s, high));
    assert(native_config_load_file(&untouched, high, true) && untouched.wheel_step == 50);
    fail_sync_call = 0;
    unsetenv("LGNOME_NATIVE_SETTINGS_PATH");

    char moved[1024];
    make_path(moved, sizeof(moved), settings_dir, "moved");
    assert(rename(private_home, moved) == 0 && symlink(primary_dir, private_home) == 0);
    assert(!native_config_load_persisted(&s, false));
    assert(unlink(private_home) == 0 && rename(moved, private_home) == 0);
    assert(setenv("HOME", "/", 1) == 0);
    assert(!native_config_load_persisted(&s, false));
    assert(!native_config_save_persisted(&s));

    assert(unlink(home_file) == 0);
    assert(rmdir(private_home) == 0);
    assert(rmdir(settings_dir) == 0);
    assert(unlink(high) == 0 && unlink(low) == 0);
    assert(rmdir(primary_dir) == 0 && rmdir(alternate_dir) == 0);
    assert(rmdir(home) == 0 && rmdir(root) == 0);
    puts("config-storage: all checks passed");
    return 0;
}
