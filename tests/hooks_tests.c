/*
 * hooks_tests.c — Host-side tests for startup patching and boot hook behavior.
 */

#include "hooks.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int stub_daemon_running;
static int stub_spawn_calls;

int ipc_daemon_running(void) {
    return stub_daemon_running;
}

int daemon_spawn_background(const char *self_path) {
    (void)self_path;
    stub_spawn_calls++;
    stub_daemon_running = 1;
    return 0;
}

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
    CHECK(size >= 0, "could not tell file");
    CHECK(fseek(f, 0, SEEK_SET) == 0, "could not rewind file");

    buf = (char *)malloc((size_t)size + 1);
    CHECK(buf != NULL, "could not allocate file buffer");
    CHECK(fread(buf, 1, (size_t)size, f) == (size_t)size, "could not read file");
    fclose(f);
    buf[size] = '\0';
    return buf;
}

static char *capture_startup_env(void) {
    FILE *f;
    long size;
    char *buf;

    f = tmpfile();
    CHECK(f != NULL, "could not create tmpfile");
    CHECK(hooks_write_startup_env(f) == 0, "hooks_write_startup_env failed");
    CHECK(fflush(f) == 0, "could not flush tmpfile");
    CHECK(fseek(f, 0, SEEK_END) == 0, "could not seek tmpfile end");
    size = ftell(f);
    CHECK(size >= 0, "could not tell tmpfile");
    CHECK(fseek(f, 0, SEEK_SET) == 0, "could not rewind tmpfile");

    buf = (char *)malloc((size_t)size + 1);
    CHECK(buf != NULL, "could not allocate env buffer");
    CHECK(fread(buf, 1, (size_t)size, f) == (size_t)size, "could not read tmpfile");
    fclose(f);
    buf[size] = '\0';
    return buf;
}

static char *run_shell_script(const char *script_path) {
    int pipefd[2];
    pid_t pid;
    char chunk[256];
    size_t total = 0;
    size_t capacity = 256;
    char *output;
    ssize_t nread;
    int status;

    CHECK(pipe(pipefd) == 0, "pipe failed");

    pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        close(pipefd[0]);
        CHECK(dup2(pipefd[1], STDOUT_FILENO) >= 0, "dup2 failed");
        close(pipefd[1]);
        execl("/bin/sh", "sh", script_path, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    output = (char *)malloc(capacity);
    CHECK(output != NULL, "could not allocate script output buffer");

    while ((nread = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        if (total + (size_t)nread + 1 > capacity) {
            char *grown;
            capacity = (total + (size_t)nread + 1) * 2;
            grown = (char *)realloc(output, capacity);
            CHECK(grown != NULL, "could not grow script output buffer");
            output = grown;
        }
        memcpy(output + total, chunk, (size_t)nread);
        total += (size_t)nread;
    }

    CHECK(nread == 0, "script output read failed");
    close(pipefd[0]);
    CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "shell evaluation should succeed");

    output[total] = '\0';
    return output;
}

static char *evaluate_startup_env_script(const char *root, const char *env_text) {
    char script_path[PATH_MAX];
    FILE *f;

    snprintf(script_path, sizeof(script_path), "%s/eval-startup-env.sh", root);
    f = fopen(script_path, "wb");
    CHECK(f != NULL, "could not open startup env script");
    CHECK(fputs("#!/bin/sh\n", f) >= 0, "could not write startup env shebang");
    CHECK(fputs("LD_PRELOAD='/existing/preload.so'\n", f) >= 0,
          "could not seed LD_PRELOAD");
    CHECK(fputs(env_text, f) >= 0, "could not write startup env body");
    CHECK(fputs("printf '%s' \"$LD_PRELOAD\"\n", f) >= 0,
          "could not write startup env probe");
    fclose(f);
    CHECK(chmod(script_path, 0755) == 0, "could not chmod startup env script");

    return run_shell_script(script_path);
}

static int count_substring(const char *haystack, const char *needle) {
    int count = 0;
    const char *p = haystack;
    size_t needle_len = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }

    return count;
}

static void write_stub_overlay(const char *pak_dir) {
    char overlay_path[PATH_MAX];
    FILE *f;

    snprintf(overlay_path, sizeof(overlay_path), "%s/varnish_overlay.so", pak_dir);
    mkdirp(pak_dir);
    f = fopen(overlay_path, "wb");
    CHECK(f != NULL, "could not create overlay stub");
    fclose(f);
}

static void configure_platform_env_named(const char *root, const char *platform,
                                         const char *pak_dir_name,
                                         char *startup_path, size_t startup_size,
                                         char *boot_path, size_t boot_size,
                                         char *pak_dir, size_t pak_dir_size) {
    char sdcard[PATH_MAX];
    char userdata[PATH_MAX];

    snprintf(sdcard, sizeof(sdcard), "%s/sdcard", root);
    snprintf(userdata, sizeof(userdata), "%s/userdata/%s", root, platform);
    snprintf(pak_dir, pak_dir_size, "%s/Tools/%s/%s", sdcard, platform, pak_dir_name);
    snprintf(startup_path, startup_size, "%s/.tmp_update/%s.sh", sdcard, platform);
    snprintf(boot_path, boot_size, "%s/.hooks/boot.d/varnish.sync.sh", userdata);

    mkdirp(userdata);
    write_stub_overlay(pak_dir);

    CHECK(setenv("SDCARD_PATH", sdcard, 1) == 0, "setenv SDCARD_PATH failed");
    CHECK(setenv("USERDATA_PATH", userdata, 1) == 0, "setenv USERDATA_PATH failed");
    CHECK(setenv("PLATFORM", platform, 1) == 0, "setenv PLATFORM failed");
    CHECK(setenv("PAK_DIR", pak_dir, 1) == 0, "setenv PAK_DIR failed");
}

static void configure_platform_env(const char *root, const char *platform,
                                   char *startup_path, size_t startup_size,
                                   char *boot_path, size_t boot_size) {
    char pak_dir[PATH_MAX];

    configure_platform_env_named(root, platform, "Varnish.pak",
                                 startup_path, startup_size,
                                 boot_path, boot_size,
                                 pak_dir, sizeof(pak_dir));
}

static void run_platform_case(const char *root, const char *platform) {
    char startup_path[PATH_MAX];
    char boot_path[PATH_MAX];
    char *content;
    char *env_text;
    int rc;

    configure_platform_env(root, platform, startup_path, sizeof(startup_path),
                           boot_path, sizeof(boot_path));

    CHECK(access(startup_path, F_OK) == 0, "startup fixture missing");
    CHECK(!hooks_is_enabled(), "enabled marker should start absent");
    CHECK(!hooks_startup_installed(), "startup patch should start absent");
    CHECK(!hooks_boot_installed(), "boot hook should start absent");

    CHECK(hooks_set_enabled(true) == 0, "hooks_set_enabled(true) failed");
    CHECK(hooks_is_enabled(), "enabled marker should be present");

    CHECK(hooks_install_startup() == 0, "hooks_install_startup failed");
    CHECK(hooks_install_startup() == 0, "hooks_install_startup should be idempotent");
    CHECK(hooks_startup_installed(), "startup patch should be installed");

    content = read_file(startup_path);
    CHECK(count_substring(content, "VARNISH STARTUP") == 2,
          "startup markers should appear exactly once");
    CHECK(count_substring(content, "--startup-env") == 1,
          "startup helper should only be injected once");
    free(content);

    CHECK(hooks_uninstall_startup() == 0, "hooks_uninstall_startup failed");
    CHECK(hooks_uninstall_startup() == 0, "hooks_uninstall_startup should be idempotent");
    CHECK(!hooks_startup_installed(), "startup patch should be removed");

    CHECK(hooks_install_boot() == 0, "hooks_install_boot failed");
    CHECK(hooks_install_boot() == 0, "hooks_install_boot should be idempotent");
    CHECK(hooks_boot_installed(), "boot hook should be installed");
    CHECK(access(boot_path, F_OK) == 0, "boot hook file missing");

    content = read_file(boot_path);
    CHECK(strstr(content, "--boot-hook") != NULL, "boot hook should call --boot-hook");
    free(content);

    CHECK(hooks_uninstall_boot() == 0, "hooks_uninstall_boot failed");
    CHECK(hooks_uninstall_boot() == 0, "hooks_uninstall_boot should be idempotent");
    CHECK(!hooks_boot_installed(), "boot hook should be removed");

    CHECK(hooks_install_boot() == 0, "hooks_install_boot reinstall failed");
    CHECK(hooks_uninstall_startup() == 0, "startup uninstall before env capture failed");
    CHECK(!hooks_startup_installed(), "startup patch should be absent before env capture");

    env_text = capture_startup_env();
    CHECK(hooks_startup_installed(), "hooks_write_startup_env should re-install startup patch");
    CHECK(strstr(env_text, "LD_PRELOAD") != NULL, "startup env should export LD_PRELOAD");
    CHECK(strstr(env_text, "varnish_overlay.so") != NULL, "startup env should reference overlay");
    free(env_text);

    CHECK(hooks_set_enabled(false) == 0, "hooks_set_enabled(false) failed");
    CHECK(!hooks_is_enabled(), "enabled marker should be removed");

    env_text = capture_startup_env();
    CHECK(env_text[0] == '\0', "startup env should be empty when disabled");
    free(env_text);

    CHECK(hooks_set_enabled(true) == 0, "re-enable marker failed");
    CHECK(hooks_uninstall_startup() == 0, "startup uninstall before boot check failed");

    stub_daemon_running = 0;
    stub_spawn_calls = 0;
    rc = hooks_boot_check("varnish");
    CHECK(rc == 2, "boot check should request reboot after repairing startup patch");
    CHECK(hooks_startup_installed(), "boot check should repair missing startup patch");
    CHECK(stub_spawn_calls == 0, "boot check should not spawn daemon when rebooting");

    stub_daemon_running = 0;
    stub_spawn_calls = 0;
    rc = hooks_boot_check("varnish");
    CHECK(rc == 0, "boot check should succeed with repaired startup patch");
    CHECK(stub_spawn_calls == 1, "boot check should start daemon when needed");

    stub_daemon_running = 1;
    stub_spawn_calls = 0;
    rc = hooks_boot_check("varnish");
    CHECK(rc == 0, "boot check should succeed when daemon is already running");
    CHECK(stub_spawn_calls == 0, "boot check should not spawn a second daemon");

    CHECK(hooks_set_enabled(false) == 0, "final disable failed");
    stub_daemon_running = 0;
    stub_spawn_calls = 0;
    rc = hooks_boot_check("varnish");
    CHECK(rc == 0, "boot check should no-op when disabled");
    CHECK(stub_spawn_calls == 0, "disabled boot check should not spawn daemon");
}

static void test_startup_env_shell_escaping(const char *root) {
    char startup_path[PATH_MAX];
    char boot_path[PATH_MAX];
    char pak_dir[PATH_MAX];
    char expected[PATH_MAX + 64];
    char *env_text;
    char *evaluated;

    configure_platform_env_named(root, "tg5040", "Varnish '$HOME test.pak",
                                 startup_path, sizeof(startup_path),
                                 boot_path, sizeof(boot_path),
                                 pak_dir, sizeof(pak_dir));

    CHECK(hooks_set_enabled(true) == 0, "enable marker for escape test failed");
    CHECK(hooks_uninstall_startup() == 0, "startup uninstall for escape test failed");
    CHECK(hooks_uninstall_boot() == 0, "boot uninstall for escape test failed");

    env_text = capture_startup_env();
    evaluated = evaluate_startup_env_script(root, env_text);
    snprintf(expected, sizeof(expected), "%s/varnish_overlay.so:/existing/preload.so", pak_dir);

    CHECK(strcmp(evaluated, expected) == 0,
          "startup env should preserve literal overlay path contents");

    free(env_text);
    free(evaluated);
    CHECK(hooks_set_enabled(false) == 0, "disable marker for escape test failed");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <tmp-root>\n", argv[0]);
        return 1;
    }

    run_platform_case(argv[1], "tg5040");
    run_platform_case(argv[1], "tg5050");
    test_startup_env_shell_escaping(argv[1]);

    puts("hooks_tests: ok");
    return 0;
}
