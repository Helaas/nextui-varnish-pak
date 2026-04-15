/*
 * hotkeys_tests.c — Host-side tests for hotkey config and logic helpers.
 */

#include "hotkeys.h"
#include "manual.h"
#include "screenshot.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "could not open file for writing");
    CHECK(fputs(content, f) >= 0, "could not write file");
    fclose(f);
}

static uint32_t parse_mask_or_fail(const char *text) {
    uint32_t mask = 0u;
    CHECK(hotkeys_parse_mask(text, &mask) == 0, "could not parse mask");
    return mask;
}

static void configure_env(const char *root) {
    char userdata[PATH_MAX];
    char shared[PATH_MAX];

    snprintf(userdata, sizeof(userdata), "%s/userdata/tg5040", root);
    snprintf(shared, sizeof(shared), "%s/shared", root);
    mkdirp(userdata);
    mkdirp(shared);

    CHECK(setenv("USERDATA_PATH", userdata, 1) == 0, "setenv USERDATA_PATH failed");
    CHECK(setenv("SHARED_USERDATA_PATH", shared, 1) == 0,
          "setenv SHARED_USERDATA_PATH failed");
    CHECK(setenv("SDCARD_PATH", "/mnt/SDCARD", 1) == 0, "setenv SDCARD_PATH failed");
    CHECK(setenv("PLATFORM", "tg5040", 1) == 0, "setenv PLATFORM failed");
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

    CHECK(hotkeys_parse_mask("f2 + f1", &mask) == 0,
          "parse should accept function buttons");
    CHECK(hotkeys_format_mask(mask, formatted, sizeof(formatted)) == 0,
          "format mask failed for function button chord");
    CHECK(strcmp(formatted, "F1+F2") == 0,
          "format should keep function buttons in canonical order");

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
    saved.manual_mask = parse_mask_or_fail("L2+R2");
    saved.record_mask = parse_mask_or_fail("L1+R2");

    CHECK(hotkeys_save_config(&saved) == 0, "save config failed");

    snprintf(path, sizeof(path), "%s/userdata/tg5040/Varnish/keybinds.txt", root);
    content = read_file(path);
    CHECK(strstr(content, "screenshot=L1+R1") != NULL,
          "saved config should contain normalized screenshot binding");
    CHECK(strstr(content, "manual=L2+R2") != NULL,
          "saved config should contain normalized manual binding");
    CHECK(strstr(content, "record=L1+R2") != NULL,
          "saved config should contain normalized record binding");
    free(content);

    hotkeys_config_init(&loaded);
    CHECK(hotkeys_load_config(&loaded) == 0, "load config failed");
    CHECK(loaded.screenshot_mask == saved.screenshot_mask,
          "loaded config should match saved config");
    CHECK(loaded.manual_mask == saved.manual_mask,
          "loaded manual config should match saved config");
    CHECK(loaded.record_mask == saved.record_mask,
          "loaded record config should match saved config");
}

static void test_config_errors(const char *root) {
    FILE *f;
    char path[PATH_MAX];
    varnish_hotkey_config config;

    snprintf(path, sizeof(path), "%s/userdata/tg5040/Varnish/keybinds.txt", root);
    f = fopen(path, "wb");
    CHECK(f != NULL, "could not write malformed config");
    fputs("screenshot=POWER+L1\nmanual=L2+R2\nrecord=L1+R2\n", f);
    fclose(f);

    hotkeys_config_init(&config);
    CHECK(hotkeys_load_config(&config) != 0, "malformed config should return error");
    CHECK(config.screenshot_mask == 0u, "malformed binding should be disabled");
    CHECK(config.manual_mask == parse_mask_or_fail("L2+R2"),
          "valid manual binding should still load");
    CHECK(config.record_mask == parse_mask_or_fail("L1+R2"),
          "valid record binding should still load");
}

static void test_logic_one_shot(void) {
    varnish_hotkey_logic logic;
    uint32_t screenshot_mask = parse_mask_or_fail("L1+R1");
    uint32_t manual_mask = parse_mask_or_fail("L2+R2");
    uint32_t record_mask = parse_mask_or_fail("L1+R2");

    hotkeys_logic_init(&logic, screenshot_mask, manual_mask, record_mask);

    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "release should not trigger");
    CHECK(hotkeys_logic_update(&logic, screenshot_mask) == VARNISH_HOTKEY_ACTION_SCREENSHOT,
          "exact chord should trigger once");
    CHECK(hotkeys_logic_update(&logic, screenshot_mask) == VARNISH_HOTKEY_ACTION_NONE,
          "held chord should not retrigger");
    CHECK(hotkeys_logic_update(&logic, hotkeys_button_bit(VARNISH_HOTKEY_BUTTON_L1)) ==
              VARNISH_HOTKEY_ACTION_NONE,
          "partial release should not retrigger");
    CHECK(hotkeys_logic_update(&logic, screenshot_mask) == VARNISH_HOTKEY_ACTION_NONE,
          "re-press before full release should stay latched");
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "full release should reset latch");
    CHECK(hotkeys_logic_update(&logic, screenshot_mask) == VARNISH_HOTKEY_ACTION_SCREENSHOT,
          "chord should trigger again after full release");
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "second release should reset for manual action");
    CHECK(hotkeys_logic_update(&logic, manual_mask) == VARNISH_HOTKEY_ACTION_MANUAL,
          "manual chord should trigger its own action");
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "manual release should reset for record action");
    CHECK(hotkeys_logic_update(&logic, record_mask) == VARNISH_HOTKEY_ACTION_RECORD_TOGGLE,
          "record chord should trigger its own action");
}

static void test_logic_pause_resume(void) {
    varnish_hotkey_logic logic;
    uint32_t screenshot_mask = parse_mask_or_fail("L1+R1");
    uint32_t manual_mask = parse_mask_or_fail("L2+R2");
    uint32_t record_mask = parse_mask_or_fail("L1+R2");

    hotkeys_logic_init(&logic, screenshot_mask, manual_mask, record_mask);
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "initial release should clear wait state");

    hotkeys_logic_set_paused(&logic, true);
    CHECK(hotkeys_logic_update(&logic, manual_mask) == VARNISH_HOTKEY_ACTION_NONE,
          "paused logic should not trigger");
    hotkeys_logic_set_paused(&logic, false);
    CHECK(hotkeys_logic_update(&logic, manual_mask) == VARNISH_HOTKEY_ACTION_NONE,
          "resume should wait for release before re-arming");
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "release should re-arm after resume");
    CHECK(hotkeys_logic_update(&logic, manual_mask) == VARNISH_HOTKEY_ACTION_MANUAL,
          "manual chord should trigger after resume and release");
    CHECK(hotkeys_logic_update(&logic, 0u) == VARNISH_HOTKEY_ACTION_NONE,
          "release should re-arm after manual");
    CHECK(hotkeys_logic_update(&logic, record_mask) == VARNISH_HOTKEY_ACTION_RECORD_TOGGLE,
          "record chord should trigger after resume flow");
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

static void test_manual_download_dir(const char *root) {
    char settings_dir[PATH_MAX];
    char settings_path[PATH_MAX];
    char loaded[PATH_MAX];

    snprintf(settings_dir, sizeof(settings_dir), "%s/shared/ScrapeGoat", root);
    mkdirp(settings_dir);
    snprintf(settings_path, sizeof(settings_path), "%s/settings.json", settings_dir);
    write_file(settings_path,
               "{\n"
               "  \"manual_download_dir\": \"/mnt/SDCARD/Reader/Manuals\"\n"
               "}\n");

    CHECK(manual_read_download_dir(loaded, sizeof(loaded)) == 0,
          "manual download dir should load");
    CHECK(strcmp(loaded, "/mnt/SDCARD/Reader/Manuals") == 0,
          "loaded manual download dir should match config");
}

static void test_manual_lookup_resolution(const char *root) {
    char manual_root[PATH_MAX];
    char gb_dir[PATH_MAX];
    char manual_path[PATH_MAX];
    varnish_manual_lookup lookup;

    snprintf(manual_root, sizeof(manual_root), "%s/manuals", root);
    snprintf(gb_dir, sizeof(gb_dir), "%s/GB", manual_root);
    mkdirp(gb_dir);
    snprintf(manual_path, sizeof(manual_path), "%s/Home Alone (USA, Europe).pdf", gb_dir);
    write_file(manual_path, "pdf");

    CHECK(manual_build_lookup_from_rom(
              "/mnt/SDCARD/Roms/GB (GB)/Home Alone (USA, Europe).gb",
              manual_root, &lookup) == 0,
          "manual lookup should resolve from ROM path");
    CHECK(strcmp(lookup.system_tag, "GB") == 0, "system tag should match folder tag");
    CHECK(strcmp(lookup.display_name, "Home Alone (USA, Europe)") == 0,
          "display name should strip the ROM extension");
    CHECK(strcmp(lookup.manual_path, manual_path) == 0,
          "manual path should follow ScrapeGoat naming");
    CHECK(strcmp(lookup.browse_dir, gb_dir) == 0,
          "browse dir should prefer the system manual folder");
    CHECK(lookup.exact_match, "exact manual file should be detected");
}

static void test_minarch_cmdline_parse(void) {
    const char cmdline[] =
        "/mnt/SDCARD/.system/tg5040/bin/minarch.elf\0"
        "/mnt/SDCARD/Emus/libretro/core.so\0"
        "/mnt/SDCARD/Roms/GB (GB)/Home Alone (USA, Europe).gb\0";
    char rom_path[PATH_MAX];

    CHECK(manual_parse_minarch_cmdline(cmdline, sizeof(cmdline),
                                       rom_path, sizeof(rom_path)) == 0,
          "minarch cmdline should parse");
    CHECK(strcmp(rom_path, "/mnt/SDCARD/Roms/GB (GB)/Home Alone (USA, Europe).gb") == 0,
          "parsed ROM path should match the last non-core argument");
}

static void test_manual_browser_state(const char *root) {
    char state_dir[PATH_MAX];
    char config_path[PATH_MAX];
    char history_path[PATH_MAX];
    char temp_state_dir[PATH_MAX];
    char temp_config[PATH_MAX];
    char temp_history[PATH_MAX];
    char *config_json;
    char *history_json;

    snprintf(state_dir, sizeof(state_dir), "%s/shared/SDLReader", root);
    mkdirp(state_dir);
    snprintf(config_path, sizeof(config_path), "%s/config.json", state_dir);
    snprintf(history_path, sizeof(history_path), "%s/reading_history.json", state_dir);
    write_file(config_path,
               "{\n"
               "  \"fontSize\": 12,\n"
               "  \"lastBrowseDirectory\": \"/old/path\"\n"
               "}\n");
    write_file(history_path, "{\"history\":[]}\n");

    CHECK(manual_prepare_browser_state(state_dir, "/mnt/SDCARD/Reader/Manuals/GB",
                                       temp_state_dir, sizeof(temp_state_dir)) == 0,
          "browser state should prepare");

    snprintf(temp_config, sizeof(temp_config), "%s/config.json", temp_state_dir);
    snprintf(temp_history, sizeof(temp_history), "%s/reading_history.json", temp_state_dir);
    config_json = read_file(temp_config);
    history_json = read_file(temp_history);
    CHECK(strstr(config_json, "\"fontSize\": 12") != NULL,
          "temp config should preserve existing settings");
    CHECK(strstr(config_json,
                 "\"lastBrowseDirectory\": \"/mnt/SDCARD/Reader/Manuals/GB\"") != NULL,
          "temp config should force the requested browse directory");
    CHECK(strstr(history_json, "\"history\":[]") != NULL,
          "temp history should be copied");
    free(config_json);
    free(history_json);

    manual_cleanup_temp_state(temp_state_dir);
}

static void test_manual_session_poll(void) {
    int pipefd[2];
    pid_t minarch_pid;
    pid_t reader_pid;
    int status = 0;
    char byte = '\0';
    varnish_manual_session session;

    CHECK(pipe(pipefd) == 0, "could not create session pipe");

    minarch_pid = fork();
    CHECK(minarch_pid >= 0, "could not fork minarch test child");
    if (minarch_pid == 0) {
        close(pipefd[0]);
        raise(SIGSTOP);
        (void)write(pipefd[1], "r", 1);
        pause();
        _exit(0);
    }

    close(pipefd[1]);
    CHECK(waitpid(minarch_pid, &status, WUNTRACED) == minarch_pid,
          "could not wait for stopped minarch child");
    CHECK(WIFSTOPPED(status), "minarch child should be stopped");

    reader_pid = fork();
    CHECK(reader_pid >= 0, "could not fork reader test child");
    if (reader_pid == 0) {
        usleep(100000);
        _exit(0);
    }

    manual_session_init(&session);
    CHECK(manual_session_begin(&session, minarch_pid, reader_pid, "") == 0,
          "manual session should begin");
    CHECK(manual_session_poll(&session) == 0,
          "manual session should stay active while reader runs");

    for (int i = 0; i < 20; i++) {
        if (manual_session_poll(&session) == 1)
            break;
        usleep(25000);
    }
    CHECK(!session.active, "manual session should finish after reader exits");
    CHECK(read(pipefd[0], &byte, 1) == 1 && byte == 'r',
          "manual session should resume the stopped minarch child");

    kill(minarch_pid, SIGTERM);
    waitpid(minarch_pid, &status, 0);
    close(pipefd[0]);
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
    test_manual_download_dir(argv[1]);
    test_manual_lookup_resolution(argv[1]);
    test_minarch_cmdline_parse();
    test_manual_browser_state(argv[1]);
    test_manual_session_poll();

    puts("hotkeys_tests: ok");
    return 0;
}
