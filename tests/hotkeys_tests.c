/*
 * hotkeys_tests.c — Host-side tests for hotkey config and logic helpers.
 */

#include "hotkeys.h"
#include "screenshot.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

#define CHECK(cond, msg) do { \
    if (!(cond)) fail(msg); \
} while (0)

static void mkdirp(const char *path) {
    char tmp[PATH_MAX];
    char *p;

    if (!path || !path[0]) return;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static char *read_file(const char *path) {
    FILE *f;
    long size;
    char *buf;

    f = fopen(path, "rb");
    CHECK(f != NULL, "could not open file");
    CHECK(fseek(f, 0, SEEK_END) == 0, "could not seek file");
    size = ftell(f);
    CHECK(size >= 0, "could not tell file size");
    CHECK(fseek(f, 0, SEEK_SET) == 0, "could not rewind file");

    buf = (char *)malloc((size_t)size + 1);
    CHECK(buf != NULL, "could not allocate file buffer");
    CHECK(fread(buf, 1, (size_t)size, f) == (size_t)size, "could not read file");
    fclose(f);
    buf[size] = '\0';
    return buf;
}

static uint32_t parse_mask_or_fail(const char *text) {
    uint32_t mask = 0u;
    CHECK(hotkeys_parse_mask(text, &mask) == 0, "could not parse mask");
    return mask;
}

static void configure_env(const char *root) {
    char userdata[PATH_MAX];

    snprintf(userdata, sizeof(userdata), "%s/userdata/tg5040", root);
    mkdirp(userdata);

    CHECK(setenv("USERDATA_PATH", userdata, 1) == 0, "setenv USERDATA_PATH failed");
}

static void test_parse_and_format(void) {
    uint32_t mask = 0u;
    char formatted[64];

    CHECK(hotkeys_parse_mask("R1 + L1", &mask) == 0, "parse should accept spacing");
    CHECK(hotkeys_mask_button_count(mask) == 2, "expected two-button chord");
    CHECK(hotkeys_format_mask(mask, formatted, sizeof(formatted)) == 0,
          "format mask failed");
    CHECK(strcmp(formatted, "L1+R1") == 0, "format should normalize button order");

    CHECK(hotkeys_parse_mask("l2+start+menu", &mask) == 0,
          "parse should accept lowercase names");
    CHECK(hotkeys_format_mask(mask, formatted, sizeof(formatted)) == 0,
          "format mask failed for 3-button chord");
    CHECK(strcmp(formatted, "L2+START+MENU") == 0,
          "format should keep canonical order");

    CHECK(hotkeys_parse_mask("POWER+L1", &mask) != 0,
          "parse should reject unsupported buttons");
    CHECK(hotkeys_parse_mask("A", &mask) != 0,
          "parse should reject single-button bindings");
    CHECK(hotkeys_parse_mask("A+B+X+Y+L1", &mask) != 0,
          "parse should reject 5-button bindings");
}

static void test_config_roundtrip(const char *root) {
    varnish_hotkey_config saved;
    varnish_hotkey_config loaded;
    char path[PATH_MAX];
    char *content;

    (void)root;
    hotkeys_config_init(&saved);
    saved.screenshot_mask = parse_mask_or_fail("L1+R1");

    CHECK(hotkeys_save_config(&saved) == 0, "save config failed");

    snprintf(path, sizeof(path), "%s/userdata/tg5040/Varnish/keybinds.txt", root);
    content = read_file(path);
    CHECK(strstr(content, "screenshot=L1+R1") != NULL,
          "saved config should contain normalized screenshot binding");
    free(content);

    hotkeys_config_init(&loaded);
    CHECK(hotkeys_load_config(&loaded) == 0, "load config failed");
    CHECK(loaded.screenshot_mask == saved.screenshot_mask,
          "loaded config should match saved config");
}

static void test_config_errors(const char *root) {
    FILE *f;
    char path[PATH_MAX];
    varnish_hotkey_config config;

    snprintf(path, sizeof(path), "%s/userdata/tg5040/Varnish/keybinds.txt", root);
    f = fopen(path, "wb");
    CHECK(f != NULL, "could not write malformed config");
    fputs("screenshot=POWER+L1\n", f);
    fclose(f);

    hotkeys_config_init(&config);
    CHECK(hotkeys_load_config(&config) != 0, "malformed config should return error");
    CHECK(config.screenshot_mask == 0u, "malformed binding should be disabled");
}

static void test_logic_one_shot(void) {
    varnish_hotkey_logic logic;
    uint32_t mask = parse_mask_or_fail("L1+R1");

    hotkeys_logic_init(&logic, mask);

    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "release should not trigger");
    CHECK(hotkeys_logic_update(&logic, mask) == VARNISH_HOTKEY_ACTION_SCREENSHOT,
          "exact chord should trigger once");
    CHECK(hotkeys_logic_update(&logic, mask) == VARNISH_HOTKEY_ACTION_NONE,
          "held chord should not retrigger");
    CHECK(hotkeys_logic_update(&logic, hotkeys_button_bit(VARNISH_HOTKEY_BUTTON_L1)) ==
              VARNISH_HOTKEY_ACTION_NONE,
          "partial release should not retrigger");
    CHECK(hotkeys_logic_update(&logic, mask) == VARNISH_HOTKEY_ACTION_NONE,
          "re-press before full release should stay latched");
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "full release should reset latch");
    CHECK(hotkeys_logic_update(&logic, mask) == VARNISH_HOTKEY_ACTION_SCREENSHOT,
          "chord should trigger again after full release");
}

static void test_logic_pause_resume(void) {
    varnish_hotkey_logic logic;
    uint32_t mask = parse_mask_or_fail("L1+R1");

    hotkeys_logic_init(&logic, mask);
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "initial release should clear wait state");

    hotkeys_logic_set_paused(&logic, true);
    CHECK(hotkeys_logic_update(&logic, mask) == VARNISH_HOTKEY_ACTION_NONE,
          "paused logic should not trigger");
    hotkeys_logic_set_paused(&logic, false);
    CHECK(hotkeys_logic_update(&logic, mask) == VARNISH_HOTKEY_ACTION_NONE,
          "resume should wait for release before re-arming");
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "release should re-arm after resume");
    CHECK(hotkeys_logic_update(&logic, mask) == VARNISH_HOTKEY_ACTION_SCREENSHOT,
          "chord should trigger after resume and release");
}

static void test_screenshot_path_collision(const char *root) {
    char dir[PATH_MAX];
    char first[PATH_MAX];
    char second[PATH_MAX];
    FILE *f;
    time_t fixed_time = 1712620800; /* 2024-04-09 00:00:00 UTC */

    snprintf(dir, sizeof(dir), "%s/screenshots", root);
    mkdirp(dir);

    CHECK(setenv("TZ", "UTC", 1) == 0, "setenv TZ failed");
    tzset();

    CHECK(screenshot_resolve_output_path(dir, fixed_time, first, sizeof(first)) == 0,
          "first screenshot path should resolve");
    CHECK(strstr(first, "varnish-20240409-000000.png") != NULL,
          "unexpected first screenshot filename");

    f = fopen(first, "wb");
    CHECK(f != NULL, "could not create first screenshot file");
    fclose(f);

    CHECK(screenshot_resolve_output_path(dir, fixed_time, second, sizeof(second)) == 0,
          "second screenshot path should resolve");
    CHECK(strstr(second, "varnish-20240409-000000-01.png") != NULL,
          "collision path should append a numeric suffix");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <tmp-root>\n", argv[0]);
        return 1;
    }

    configure_env(argv[1]);
    test_parse_and_format();
    test_config_roundtrip(argv[1]);
    test_config_errors(argv[1]);
    test_logic_one_shot();
    test_logic_pause_resume();
    test_screenshot_path_collision(argv[1]);

    puts("hotkeys_tests: ok");
    return 0;
}
