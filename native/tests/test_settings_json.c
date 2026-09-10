#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "native_config_storage.h"
#include "native_config.h"
#include "native_config_launch.h"
#include "settings_json.h"

static NativeSettings make_defaults(void) {
    NativeSettings settings;
    native_settings_defaults(&settings);
    return settings;
}

static void test_defaults(void) {
    NativeSettings s = make_defaults();
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].host, "127.0.0.1") == 0);
    assert(s.sessions[NATIVE_SESSION_SLOT_YELLOW].host[0] == '\0');
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        assert(s.sessions[i].name[0] == '\0');
        assert(s.sessions[i].port == 3389);
        assert(s.sessions[i].fps == 60);
        assert(s.sessions[i].desktop_width == 3840 && s.sessions[i].desktop_height == 2160);
    }
    assert(s.wheel_step == 60 && s.wheel_scroll_divisor == 1);
    assert(!s.camera_enabled && s.camera_device_id[0] == '\0');
    assert(s.camera_width == 640 && s.camera_height == 480 &&
           s.camera_fps == 15);
    assert(!s.audio_input_enabled && s.audio_input_device_id[0] == '\0');
    assert(s.audio_input_gain_db == 0);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        assert(!s.sessions[i].camera_redirect);
        assert(!s.sessions[i].audio_input_redirect);
    }
}

/* A profile carries the desktop size it asks the server for, and a document written
 * before the field keeps the default rather than a zero. */
static void test_desktop_size_round_trip(void) {
    NativeSettings s = make_defaults();
    const char *json = "{ \"sessions\": [ { \"host\": \"a\", \"desktopWidth\": 1920, \"desktopHeight\": 1080 } ] }";
    assert(native_settings_apply_json(&s, json, "test"));
    assert(s.sessions[0].desktop_width == 1920 && s.sessions[0].desktop_height == 1080);
    /* Untouched profiles keep theirs. */
    assert(s.sessions[1].desktop_width == 3840 && s.sessions[1].desktop_height == 2160);

    const char *legacy = "{ \"sessions\": [ { \"host\": \"b\", \"fps\": 30 } ] }";
    assert(native_settings_apply_json(&s, legacy, "test"));
    assert(s.sessions[0].desktop_width == 1920 && s.sessions[0].desktop_height == 1080);

    /* Out of bounds is refused, leaving the previous value in place. */
    const char *bogus = "{ \"sessions\": [ { \"host\": \"c\", \"desktopWidth\": 99 } ] }";
    assert(!native_settings_apply_json(&s, bogus, "test"));
    assert(s.sessions[0].desktop_width == 1920);
}

static void test_deleted_profile_round_trip(void) {
    for (int deleted = 0; deleted < NATIVE_SETTINGS_MAX_SESSIONS; deleted++) {
        NativeSettings s = make_defaults();
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            snprintf(s.sessions[i].host, sizeof(s.sessions[i].host), "desktop-%d", i);
            s.sessions[i].desktop_width = 1920;
            s.sessions[i].desktop_height = 1080;
        }
        /* The same reset used by the HUB delete transaction and UI draft. */
        native_session_config_defaults(&s.sessions[deleted]);
        assert(s.sessions[deleted].host[0] == '\0');
        assert(native_config_validate_runtime(&s));
        char *json = NULL;
        size_t len = 0;
        FILE *file = open_memstream(&json, &len);
        assert(file && native_settings_write_json(&s, file));
        assert(fclose(file) == 0 && len > 0);
        NativeSettings loaded = make_defaults();
        assert(native_settings_apply_json(&loaded, json, "deleted-profile"));
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            assert(!native_session_connection_config_changed(&s.sessions[i], &loaded.sessions[i]));
            assert(strcmp(s.sessions[i].name, loaded.sessions[i].name) == 0);
            assert(s.sessions[i].duck_mask == loaded.sessions[i].duck_mask);
        }
        free(json);
    }
}

static void test_resolution_changes_require_save_and_reconnect(void) {
    NativeSessionConfig saved;
    native_session_config_defaults(&saved);
    NativeSessionConfig edited = saved;
    assert(!native_session_connection_config_changed(&saved, &edited));
    edited.desktop_width = 1920;
    assert(native_session_connection_config_changed(&saved, &edited));
    assert(!native_session_endpoint_changed(&saved, &edited));
    edited = saved;
    edited.desktop_height = 1080;
    assert(native_session_connection_config_changed(&saved, &edited));
    edited = saved;
    strcpy(edited.name, "New label");
    assert(!native_session_connection_config_changed(&saved, &edited));
}

static void test_legacy_flat_applies_to_green(void) {
    NativeSettings s = make_defaults();
    const char *json = "{ \"host\": \"pc.lan\", \"port\": 3390, \"username\": \"u\", \"password\": \"p\","
                       " \"domain\": \"d\", \"fps\": 30, \"wheelStep\": 40, \"audioPrebufferMs\": 0 }";
    assert(native_settings_json_has_rdp_key(json));
    assert(native_settings_apply_json(&s, json, "test"));
    NativeSessionConfig *green = &s.sessions[NATIVE_SESSION_SLOT_GREEN];
    assert(strcmp(green->host, "pc.lan") == 0);
    assert(green->port == 3390);
    assert(strcmp(green->username, "u") == 0);
    assert(strcmp(green->password, "p") == 0);
    assert(strcmp(green->domain, "d") == 0);
    assert(green->fps == 30);
    assert(s.wheel_step == 40);
    assert(s.audio_codec == NATIVE_AUDIO_CODEC_AUTO); /* deprecated buffer value is ignored */
    /* Yellow slot untouched by a legacy document. */
    assert(s.sessions[NATIVE_SESSION_SLOT_YELLOW].host[0] == '\0');
    assert(s.sessions[NATIVE_SESSION_SLOT_YELLOW].fps == 60);
}

static void test_legacy_absent_keys_keep_values(void) {
    NativeSettings s = make_defaults();
    NativeSessionConfig *green = &s.sessions[NATIVE_SESSION_SLOT_GREEN];
    (void)snprintf(green->name, sizeof(green->name), "Keep this name");
    (void)snprintf(green->username, sizeof(green->username), "keep-me");
    assert(native_settings_apply_json(&s, "{ \"host\": \"h2\" }", "test"));
    assert(strcmp(green->name, "Keep this name") == 0);
    assert(strcmp(green->host, "h2") == 0);
    assert(strcmp(green->username, "keep-me") == 0);
}

static void test_non_string_name_is_ignored_for_compatibility(void) {
    NativeSettings s = make_defaults();
    NativeSessionConfig *green = &s.sessions[NATIVE_SESSION_SLOT_GREEN];
    (void)snprintf(green->name, sizeof(green->name), "Keep this name");

    /* Before profile labels were introduced, `name` was an unknown launch/config
     * field. Its type must not invalidate other RDP overrides in those documents. */
    assert(native_settings_apply_json(&s, "{ \"name\": null, \"host\": \"flat.lan\" }", "test"));
    assert(strcmp(green->name, "Keep this name") == 0);
    assert(strcmp(green->host, "flat.lan") == 0);

    assert(native_settings_apply_json(
        &s, "{ \"sessions\": [ { \"slot\": \"green\", \"name\": 7, \"host\": \"array.lan\" } ] }", "test"));
    assert(strcmp(green->name, "Keep this name") == 0);
    assert(strcmp(green->host, "array.lan") == 0);
}

static void test_sessions_array(void) {
    NativeSettings s = make_defaults();
    const char *json =
        "{\n"
        "  \"sessions\": [\n"
        "    { \"slot\": \"green\", \"name\": \"Studio PC\", \"host\": \"green.lan\", \"port\": 3389,"
        " \"username\": \"gu\","
        " \"password\": \"gp\", \"domain\": \"\", \"fps\": 60 },\n"
        "    { \"slot\": \"yellow\", \"host\": \"yellow.lan\", \"port\": 13389, \"username\": \"yu\","
        " \"password\": \"yp\", \"domain\": \"yd\", \"fps\": 30 }\n"
        "  ],\n"
        "  \"wheelStep\": 55, \"wheelScrollDivisor\": 2, \"audioPrebufferMs\": 150\n"
        "}\n";
    assert(native_settings_json_has_rdp_key(json));
    assert(native_settings_apply_json(&s, json, "test"));
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].name, "Studio PC") == 0);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].host, "green.lan") == 0);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].username, "gu") == 0);
    assert(s.sessions[NATIVE_SESSION_SLOT_GREEN].fps == 60);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_YELLOW].host, "yellow.lan") == 0);
    assert(s.sessions[NATIVE_SESSION_SLOT_YELLOW].port == 13389);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_YELLOW].password, "yp") == 0);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_YELLOW].domain, "yd") == 0);
    assert(s.sessions[NATIVE_SESSION_SLOT_YELLOW].fps == 30);
    assert(s.sessions[NATIVE_SESSION_SLOT_YELLOW].name[0] == '\0'); /* optional in old files */
    assert(s.wheel_step == 55 && s.wheel_scroll_divisor == 2);
}

static void test_session_slot_tags_out_of_order(void) {
    NativeSettings s = make_defaults();
    const char *json = "{ \"sessions\": ["
                       " { \"slot\": \"yellow\", \"host\": \"y.lan\" },"
                       " { \"slot\": \"green\", \"host\": \"g.lan\" } ] }";
    assert(native_settings_apply_json(&s, json, "test"));
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].host, "g.lan") == 0);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_YELLOW].host, "y.lan") == 0);
}

static void test_untagged_sessions_apply_by_index(void) {
    NativeSettings s = make_defaults();
    const char *json = "{ \"sessions\": [ { \"host\": \"a.lan\" }, { \"host\": \"b.lan\" } ] }";
    assert(native_settings_apply_json(&s, json, "test"));
    assert(strcmp(s.sessions[0].host, "a.lan") == 0);
    assert(strcmp(s.sessions[1].host, "b.lan") == 0);
}

static void test_unknown_session_slot_ignored(void) {
    NativeSettings s = make_defaults();
    const char *json = "{ \"sessions\": [ { \"slot\": \"purple\", \"host\": \"purple.lan\" } ] }";
    assert(native_settings_apply_json(&s, json, "test"));
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        assert(strstr(s.sessions[i].host, "purple.lan") == NULL);
    }
}

static void test_malformed_sessions_array_is_rejected(void) {
    NativeSettings s = make_defaults();
    NativeSettings before = s;
    /* Truncated first entry. */
    assert(!native_settings_apply_json(&s, "{ \"sessions\": [ { \"slot\": \"green\", \"host\": \"h.lan\" ", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    /* Truncated after a valid first entry: must not half-apply. */
    assert(!native_settings_apply_json(
        &s, "{ \"sessions\": [ { \"slot\": \"green\", \"host\": \"h.lan\" }, { \"slot\": \"yellow\"", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    /* Non-object entry. */
    assert(!native_settings_apply_json(&s, "{ \"sessions\": [ 42 ] }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    /* "sessions" present but not an array: must be rejected, not parsed as legacy. */
    assert(!native_settings_apply_json(&s, "{ \"sessions\": null }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(!native_settings_apply_json(&s, "{ \"sessions\": { \"slot\": \"green\" } }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(!native_settings_apply_json(&s, "{ \"sessions\":", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
}

static void test_unreadable_session_slot_tag_is_skipped(void) {
    NativeSettings s = make_defaults();
    /* Over-long tag and non-string tag must NOT fall back to by-index application. */
    assert(native_settings_apply_json(
        &s, "{ \"sessions\": [ { \"slot\": \"averyveryverylongtagname\", \"host\": \"evil.lan\" } ] }", "test"));
    assert(native_settings_apply_json(&s, "{ \"sessions\": [ { \"slot\": 7, \"host\": \"evil.lan\" } ] }", "test"));
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        assert(strstr(s.sessions[i].host, "evil.lan") == NULL);
    }
}

static void test_blue_session_slot_applies(void) {
    NativeSettings s = make_defaults();
    const char *json = "{ \"sessions\": [ { \"slot\": \"blue\", \"host\": \"blue.lan\" } ] }";
    assert(native_settings_apply_json(&s, json, "test"));
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_BLUE].host, "blue.lan") == 0);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].host, "127.0.0.1") == 0);
}

static void test_sessions_top_level_flat_keys_ignored(void) {
    /* When "sessions" is present the top-level flat lookup must not run: the substring
     * scanner would otherwise read the green object's own host/password. */
    NativeSettings s = make_defaults();
    const char *json = "{ \"host\": \"stale.lan\", \"sessions\": [ { \"slot\": \"yellow\", \"host\": \"y.lan\" } ] }";
    assert(native_settings_apply_json(&s, json, "test"));
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].host, "127.0.0.1") == 0);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_YELLOW].host, "y.lan") == 0);
}

static void test_invalid_values_leave_settings_untouched(void) {
    NativeSettings s = make_defaults();
    NativeSettings before = s;
    /* fps out of range. */
    assert(!native_settings_apply_json(&s, "{ \"fps\": 500 }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    /* Bad value inside a session-array entry. */
    assert(!native_settings_apply_json(&s, "{ \"sessions\": [ { \"slot\": \"green\", \"port\": 0 } ] }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    /* Unterminated string. */
    assert(!native_settings_apply_json(&s, "{ \"host\": \"oops }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);

    char overlong_name[NATIVE_SETTINGS_STRING_MAX + 64];
    memset(overlong_name, 'n', sizeof(overlong_name));
    overlong_name[sizeof(overlong_name) - 1] = '\0';
    char overlong_json[sizeof(overlong_name) + 32];
    (void)snprintf(overlong_json, sizeof(overlong_json), "{ \"name\": \"%s\" }", overlong_name);
    assert(!native_settings_apply_json(&s, overlong_json, "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
}

static void test_string_escapes(void) {
    NativeSettings s = make_defaults();
    const char *json =
        "{ \"name\": \"Studio \\\"A\\\"\\\\Desk\\n\", \"password\": \"a\\\"b\\\\c\\n\\u0041\" }";
    assert(native_settings_apply_json(&s, json, "test"));
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].name, "Studio \"A\"\\Desk\n") == 0);
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].password, "a\"b\\c\nA") == 0);
}

static void test_audio_codec_setting(void) {
    NativeSettings s = make_defaults();
    assert(s.audio_codec == NATIVE_AUDIO_CODEC_AUTO);
    assert(native_settings_apply_json(&s, "{ \"audioCodec\": \"pcm\" }", "test"));
    assert(s.audio_codec == NATIVE_AUDIO_CODEC_PCM);
    assert(native_settings_apply_json(&s, "{ \"audioCodec\": \"auto\" }", "test"));
    assert(s.audio_codec == NATIVE_AUDIO_CODEC_AUTO);
    assert(native_settings_apply_json(&s, "{ \"audioCodec\": \"opus\" }", "test"));
    assert(s.audio_codec == NATIVE_AUDIO_CODEC_AUTO);
    NativeSettings before = s;
    assert(!native_settings_apply_json(&s, "{ \"audioCodec\": \"flac\" }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(native_settings_json_has_rdp_key("{ \"audioCodec\": \"pcm\" }"));
}

static void test_duck_mask_setting(void) {
    NativeSettings s = make_defaults();
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        assert(s.sessions[i].duck_mask == 0); /* ducking is opt-in */
    }
    /* Legacy flat keys apply to the green slot; other slots keep their masks. */
    assert(native_settings_apply_json(&s, "{ \"duckTriggers\": 5 }", "test"));
    assert(s.sessions[NATIVE_SESSION_SLOT_GREEN].duck_mask == 5);
    assert(s.sessions[NATIVE_SESSION_SLOT_RED].duck_mask == 0);
    assert(native_settings_apply_json(
        &s, "{ \"sessions\": [ { \"slot\": \"blue\", \"duckTriggers\": 15 } ] }", "test"));
    assert(s.sessions[NATIVE_SESSION_SLOT_BLUE].duck_mask == NATIVE_SETTINGS_DUCK_MASK_ALL);
    assert(native_settings_apply_json(&s, "{ \"unrelated\": 1 }", "test"));
    assert(s.sessions[NATIVE_SESSION_SLOT_GREEN].duck_mask == 5); /* absent: keeps the current value */
    NativeSettings before = s;
    assert(!native_settings_apply_json(&s, "{ \"duckTriggers\": 16 }", "test")); /* out of range */
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(native_settings_json_has_rdp_key("{ \"duckTriggers\": 15 }"));
}

static void test_local_capture_settings(void) {
    NativeSettings s = make_defaults();
    const char *json =
        "{ \"sessions\": ["
        " { \"slot\": \"green\", \"cameraRedirect\": true,"
        "   \"audioInputRedirect\": true } ],"
        " \"cameraEnabled\": true,"
        " \"cameraDeviceId\": \"usb:046d:0943:serial\","
        " \"cameraWidth\": 1024, \"cameraHeight\": 576, \"cameraFps\": 10,"
        " \"audioInputEnabled\": true,"
        " \"audioInputDeviceId\": \"alsa:B300:0\","
        " \"audioInputGainDb\": -6 }";
    assert(native_settings_apply_json(&s, json, "test"));
    assert(s.camera_enabled);
    assert(strcmp(s.camera_device_id, "usb:046d:0943:serial") == 0);
    assert(s.camera_width == 1024 && s.camera_height == 576 &&
           s.camera_fps == 10);
    assert(s.audio_input_enabled);
    assert(strcmp(s.audio_input_device_id, "alsa:B300:0") == 0);
    assert(s.audio_input_gain_db == -6);
    assert(s.sessions[NATIVE_SESSION_SLOT_GREEN].camera_redirect);
    assert(s.sessions[NATIVE_SESSION_SLOT_GREEN].audio_input_redirect);

    NativeSettings before = s;
    assert(!native_settings_apply_json(
        &s, "{ \"audioInputGainDb\": -13 }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(!native_settings_apply_json(
        &s, "{ \"cameraFps\": 31 }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(!native_settings_apply_json(
        &s, "{ \"cameraWidth\": 641 }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(!native_settings_apply_json(
        &s, "{ \"cameraHeight\": 481 }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    /* Sub-VGA modes were retired with the software-only encode path. */
    assert(!native_settings_apply_json(
        &s, "{ \"cameraWidth\": 320 }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(!native_settings_apply_json(
        &s, "{ \"cameraHeight\": 360 }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    assert(!native_settings_apply_json(
        &s, "{ \"cameraWidth\": 2560 }", "test"));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
    /* Modes are taken from the camera now, so anything it can report has to survive
     * validation: 1080p at the top of the rate envelope, and off-preset pairs. */
    assert(native_settings_apply_json(
        &s, "{ \"cameraWidth\": 1920, \"cameraHeight\": 1080, \"cameraFps\": 30 }", "test"));
    assert(s.camera_width == 1920 && s.camera_height == 1080 && s.camera_fps == 30);
    assert(native_settings_apply_json(
        &s, "{ \"cameraWidth\": 960, \"cameraHeight\": 540, \"cameraFps\": 12 }", "test"));
    assert(s.camera_width == 960 && s.camera_height == 540 && s.camera_fps == 12);
    assert(native_settings_json_has_rdp_key(
        "{ \"cameraEnabled\": true }"));
}

static void test_has_rdp_key(void) {
    NativeSettings s = make_defaults();
    assert(!native_settings_json_has_rdp_key("{ \"unrelated\": 1 }"));
    assert(native_settings_json_has_rdp_key("{ \"sessions\": [] }"));
    assert(native_settings_json_has_rdp_key("{ \"name\": \"Studio PC\" }"));
    assert(native_settings_apply_json(&s, "{ \"name\": \"Studio PC\" }", "test"));
    assert(strcmp(s.sessions[NATIVE_SESSION_SLOT_GREEN].name, "Studio PC") == 0);
    assert(!native_settings_json_has_rdp_key("{ \"name\": null }"));
    assert(native_settings_json_has_rdp_key("{ \"wheelStep\": 60 }"));
    assert(!native_settings_json_has_rdp_key("{ \"audioPrebufferMs\": 60 }"));
}

static void test_save_load_round_trip(void) {
    NativeSettings s = make_defaults();
    (void)snprintf(s.sessions[0].name, sizeof(s.sessions[0].name), "Studio \"Red\"\\Desk\n");
    (void)snprintf(s.sessions[0].host, sizeof(s.sessions[0].host), "green \"quoted\".lan");
    (void)snprintf(s.sessions[0].username, sizeof(s.sessions[0].username), "gu");
    (void)snprintf(s.sessions[0].password, sizeof(s.sessions[0].password), "g\\p\n");
    s.sessions[0].port = 3391;
    s.sessions[0].fps = 30;
    (void)snprintf(s.sessions[1].host, sizeof(s.sessions[1].host), "yellow.lan");
    (void)snprintf(s.sessions[1].username, sizeof(s.sessions[1].username), "yu");
    (void)snprintf(s.sessions[1].password, sizeof(s.sessions[1].password), "yp");
    s.sessions[1].port = 3392;
    s.sessions[1].fps = 120;
    s.wheel_step = 33;
    s.wheel_scroll_divisor = 3;
    s.audio_codec = NATIVE_AUDIO_CODEC_PCM;
    s.sessions[0].duck_mask = 5;
    s.sessions[1].duck_mask = 15;
    s.camera_enabled = true;
    (void)snprintf(s.camera_device_id, sizeof(s.camera_device_id),
                   "usb:046d:0943:serial");
    s.camera_width = 1024;
    s.camera_height = 576;
    s.camera_fps = 15;
    s.audio_input_enabled = true;
    (void)snprintf(s.audio_input_device_id,
                   sizeof(s.audio_input_device_id), "alsa:B300:0");
    s.audio_input_gain_db = 9;
    s.sessions[0].camera_redirect = true;
    s.sessions[1].audio_input_redirect = true;

    const char *tmp_dir = getenv("TMPDIR");
    char template_path[4096];
    int path_len = snprintf(template_path, sizeof(template_path), "%s/lgnome-settings-test-XXXXXX",
                            tmp_dir && tmp_dir[0] ? tmp_dir : "/tmp");
    assert(path_len > 0 && (size_t)path_len < sizeof(template_path));
    int fd = mkstemp(template_path);
    assert(fd >= 0);
    close(fd);
    /* native_settings_save_file wants to create the file itself via rename. */
    assert(unlink(template_path) == 0);

    assert(native_settings_save_file(&s, template_path));

    FILE *file = fopen(template_path, "rb");
    assert(file);
    char buffer[8192];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[len] = '\0';
    assert(strstr(buffer, "audioPrebufferMs") == NULL);
    assert(strstr(buffer, "\"name\": \"Studio \\\"Red\\\"\\\\Desk\\n\"") != NULL);

    NativeSettings loaded = make_defaults();
    assert(native_settings_apply_json(&loaded, buffer, "round-trip"));
    assert(memcmp(&loaded.sessions, &s.sessions, sizeof(s.sessions)) == 0);
    assert(loaded.wheel_step == s.wheel_step);
    assert(loaded.wheel_scroll_divisor == s.wheel_scroll_divisor);
    assert(loaded.audio_codec == NATIVE_AUDIO_CODEC_PCM);
    assert(loaded.camera_enabled == s.camera_enabled);
    assert(strcmp(loaded.camera_device_id, s.camera_device_id) == 0);
    assert(loaded.camera_width == s.camera_width &&
           loaded.camera_height == s.camera_height &&
           loaded.camera_fps == s.camera_fps);
    assert(loaded.audio_input_enabled == s.audio_input_enabled);
    assert(strcmp(loaded.audio_input_device_id,
                  s.audio_input_device_id) == 0);
    assert(loaded.audio_input_gain_db == s.audio_input_gain_db);
    /* duck_mask is compared by the sessions memcmp above; nothing global to check. */

    assert(unlink(template_path) == 0);
}

static void test_launch_controls(void) {
    struct {
        char *json;
        bool ignore_saved;
        bool preview;
    } cases[] = {
        { "{}", false, false },
        { "{\"ignoreSavedConfig\": true}", true, false },
        { "{\"cameraPreview\": true}", false, true },
        { "{\"params\": \"{\\\"ignoreSavedConfig\\\": true, \\\"cameraPreview\\\": true}\"}", true, true },
        { "{\"launchParams\": \"{\\\"params\\\": \\\"{\\\\\\\"cameraPreview\\\\\\\": true}\\\"}\"}", false, true },
        { "{\"ignoreSavedConfig\": false, \"cameraPreview\": \"true\"}", false, false },
        { "{\"params\": \"broken\"}", false, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *argv[] = { "lgnome", cases[i].json };
        assert(native_config_launch_ignores_saved_config(2, argv) == cases[i].ignore_saved);
        assert(native_camera_preview_requested(2, argv) == cases[i].preview);
    }
    char *argv[] = { "lgnome", "--camera-preview", "--connect-slot", "yellow" };
    assert(native_camera_preview_requested(4, argv));
    assert(!native_config_launch_ignores_saved_config(4, argv));
    assert(native_connect_slot_requested(4, argv) == NATIVE_SESSION_SLOT_YELLOW);
}

static void test_removed_options_are_ignored(void) {
    NativeSettings s = make_defaults();
    s.sessions[0].desktop_width = 1920;
    s.sessions[0].desktop_height = 1080;
    NativeSettings before = s;
    const char *json = "{ \"width\": false, \"height\": null, \"audioPrebufferMs\": \"obsolete\" }";
    assert(!native_settings_json_has_rdp_key(json));
    assert(native_settings_apply_json(&s, json, "legacy"));
    char *argv[] = { "lgnome", "--width", "bad", "--height", "bad", "--audio-prebuffer-ms", "bad" };
    assert(native_config_apply_cli(&s, 7, argv));
    assert(memcmp(&s, &before, sizeof(s)) == 0);
}

static void test_capture_gates_follow_remaining_profiles(void) {
    NativeSettings s = make_defaults();
    s.sessions[0].camera_redirect = true;
    s.sessions[1].audio_input_redirect = true;
    native_settings_recompute_capture_gates(&s);
    assert(s.camera_enabled && s.audio_input_enabled);
    native_session_config_defaults(&s.sessions[0]);
    native_settings_recompute_capture_gates(&s);
    assert(!s.camera_enabled && s.audio_input_enabled);
    native_session_config_defaults(&s.sessions[1]);
    native_settings_recompute_capture_gates(&s);
    assert(!s.camera_enabled && !s.audio_input_enabled);
}

int main(void) {
    test_launch_controls();
    test_removed_options_are_ignored();
    test_capture_gates_follow_remaining_profiles();
    test_defaults();
    test_desktop_size_round_trip();
    test_deleted_profile_round_trip();
    test_resolution_changes_require_save_and_reconnect();
    test_legacy_flat_applies_to_green();
    test_legacy_absent_keys_keep_values();
    test_non_string_name_is_ignored_for_compatibility();
    test_sessions_array();
    test_session_slot_tags_out_of_order();
    test_untagged_sessions_apply_by_index();
    test_unknown_session_slot_ignored();
    test_malformed_sessions_array_is_rejected();
    test_unreadable_session_slot_tag_is_skipped();
    test_blue_session_slot_applies();
    test_sessions_top_level_flat_keys_ignored();
    test_invalid_values_leave_settings_untouched();
    test_string_escapes();
    test_audio_codec_setting();
    test_duck_mask_setting();
    test_local_capture_settings();
    test_has_rdp_key();
    test_save_load_round_trip();
    printf("test_settings_json: all tests passed\n");
    return 0;
}
