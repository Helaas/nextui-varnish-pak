#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include "recording.h"
#include "recording_shm.h"
#include "recording_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "recording_session_tests: %s\n", (msg)); \
        exit(1); \
    } \
} while (0)

typedef struct {
    char root[PATH_MAX];
    char pak_dir[PATH_MAX];
    char ffmpeg_path[PATH_MAX];
    char videos_dir[PATH_MAX];
} test_env_t;

static void path_cat(char *out, size_t out_size, const char *lhs, const char *rhs) {
    int rc = snprintf(out, out_size, "%s/%s", lhs, rhs);
    if (rc < 0 || (size_t)rc >= out_size) {
        fprintf(stderr, "recording_session_tests: path overflow\n");
        exit(1);
    }
}

static void mkdir_or_die(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        perror("mkdir");
        exit(1);
    }
}

static void write_file_or_die(const char *path, const char *contents) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    size_t len = strlen(contents);

    if (fd < 0) {
        perror("open");
        exit(1);
    }
    if (write(fd, contents, len) != (ssize_t)len) {
        perror("write");
        close(fd);
        exit(1);
    }
    close(fd);
}

static void setup_env(test_env_t *env) {
    char template_path[] = "/tmp/varnish-recording-tests-XXXXXX";
    char bin_dir[PATH_MAX];
    char sd_root[PATH_MAX];
    char videos_root[PATH_MAX];

    CHECK(env != NULL, "test env required");
    CHECK(mkdtemp(template_path) != NULL, "mkdtemp should succeed");
    snprintf(env->root, sizeof(env->root), "%s", template_path);
    path_cat(env->pak_dir, sizeof(env->pak_dir), env->root, "pak");
    path_cat(bin_dir, sizeof(bin_dir), env->pak_dir, "bin");
    path_cat(sd_root, sizeof(sd_root), env->root, "sdcard");
    path_cat(videos_root, sizeof(videos_root), sd_root, "Videos");
    path_cat(env->videos_dir, sizeof(env->videos_dir), videos_root, "Varnish");
    path_cat(env->ffmpeg_path, sizeof(env->ffmpeg_path), bin_dir, "ffmpeg");

    mkdir_or_die(env->pak_dir);
    mkdir_or_die(bin_dir);
    mkdir_or_die(sd_root);

    setenv("PAK_DIR", env->pak_dir, 1);
    setenv("SDCARD_PATH", sd_root, 1);
}

static void teardown_env(test_env_t *env) {
    char cmd[PATH_MAX + 32];

    if (!env || !env->root[0])
        return;
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", env->root);
    (void)system(cmd);
    unsetenv("PAK_DIR");
    unsetenv("SDCARD_PATH");
    unsetenv("RECORDING_TEST_MODE");
}

static void install_fake_encoder(const test_env_t *env) {
    static const char script[] =
        "#!/bin/sh\n"
        "set -eu\n"
        "mode=${RECORDING_TEST_MODE:-success}\n"
        "for last; do :; done\n"
        "out=$last\n"
        "case \"$mode\" in\n"
        "  success)\n"
        "    cat >/dev/null\n"
        "    printf 'ok\\n' > \"$out\"\n"
        "    ;;\n"
        "  partial_fail)\n"
        "    : > \"$out\"\n"
        "    cat >/dev/null\n"
        "    exit 7\n"
        "    ;;\n"
        "  hang)\n"
        "    : > \"$out\"\n"
        "    trap '' TERM\n"
        "    while :; do sleep 1; done\n"
        "    ;;\n"
        "  immediate_fail)\n"
        "    exit 9\n"
        "    ;;\n"
        "  *)\n"
        "    exit 11\n"
        "    ;;\n"
        "esac\n";

    write_file_or_die(env->ffmpeg_path, script);
}

static void push_frame(uint32_t value) {
    varnish_recording_shm_t *shm = recording_shm_get();
    uint32_t *dst;
    uint32_t slot_index = 0u;

    CHECK(shm != NULL, "recording shm should exist");
    CHECK(varnish_recording_try_acquire_producer(shm, 123, 100u),
          "test producer should acquire");
    dst = varnish_recording_begin_frame(shm, 123, 4, 4, &slot_index, NULL);
    CHECK(dst != NULL, "test frame should reserve slot");
    for (int i = 0; i < 16; i++)
        dst[i] = 0xFF000000u | value;
    varnish_recording_publish_frame(shm, slot_index);
}

static void assert_file_exists(const char *path) {
    CHECK(access(path, F_OK) == 0, "expected file to exist");
}

static void assert_file_missing(const char *path) {
    CHECK(access(path, F_OK) != 0, "expected file to be removed");
}

static void test_start_failure_cleans_up(void) {
    test_env_t env = {0};
    varnish_recording_session session;
    char message[128];

    setup_env(&env);
    install_fake_encoder(&env);
    CHECK(recording_init_transport(1024, 768) == 0, "recording shm init should work");
    recording_session_init(&session);
    setenv("RECORDING_TEST_MODE", "immediate_fail", 1);

    CHECK(recording_start(&session, message, sizeof(message)) != 0,
          "start should fail when encoder exits immediately");
    CHECK(!recording_session_active(&session), "session should stay inactive");
    CHECK(session.encoder_exit_status == 9, "start failure should capture encoder exit");
    if (session.output_path[0])
        assert_file_missing(session.output_path);

    recording_shutdown_transport();
    teardown_env(&env);
}

static void test_successful_stop_preserves_output_and_stats(void) {
    test_env_t env = {0};
    varnish_recording_session session;
    char message[128];

    setup_env(&env);
    install_fake_encoder(&env);
    CHECK(recording_init_transport(1280, 720) == 0, "recording shm init should work");
    recording_session_init(&session);
    setenv("RECORDING_TEST_MODE", "success", 1);

    CHECK(recording_start(&session, message, sizeof(message)) == 0,
          "recording should start");
    CHECK(session.output_w == 512 && session.output_h == 288,
          "session should derive aspect-correct output");
    push_frame(0x12u);
    CHECK(recording_stop(&session, message, sizeof(message)) == 0,
          "recording should stop cleanly");
    CHECK(strncmp(message, "Saved ", 6) == 0, "stop should report saved file");
    CHECK(session.frames_written == 1u, "stop should count written frames");
    CHECK(session.drop_count == 0u, "clean stop should report zero drops");
    CHECK(session.encoder_exit_status == 0, "clean stop should report zero exit");
    assert_file_exists(session.output_path);

    recording_shutdown_transport();
    teardown_env(&env);
}

static void test_nonzero_exit_removes_partial_output(void) {
    test_env_t env = {0};
    varnish_recording_session session;
    char message[128];

    setup_env(&env);
    install_fake_encoder(&env);
    CHECK(recording_init_transport(1024, 768) == 0, "recording shm init should work");
    recording_session_init(&session);
    setenv("RECORDING_TEST_MODE", "partial_fail", 1);

    CHECK(recording_start(&session, message, sizeof(message)) == 0,
          "recording should start");
    push_frame(0x34u);
    CHECK(recording_stop(&session, message, sizeof(message)) != 0,
          "non-zero encoder exit should fail stop");
    CHECK(strcmp(message, "Recording failed") == 0, "stop should report failure");
    CHECK(session.encoder_exit_status == 7, "stop should capture failing exit code");
    assert_file_missing(session.output_path);

    recording_shutdown_transport();
    teardown_env(&env);
}

static void test_timeout_kills_hung_encoder(void) {
    test_env_t env = {0};
    varnish_recording_session session;
    char message[128];

    setup_env(&env);
    install_fake_encoder(&env);
    CHECK(recording_init_transport(1024, 768) == 0, "recording shm init should work");
    recording_session_init(&session);
    setenv("RECORDING_TEST_MODE", "hang", 1);

    CHECK(recording_start(&session, message, sizeof(message)) == 0,
          "recording should start");
    CHECK(recording_stop(&session, message, sizeof(message)) != 0,
          "hung encoder should fail stop");
    CHECK(session.encoder_timed_out == 1, "hung encoder should time out");
    CHECK(session.encoder_exit_status == 137, "hung encoder should be killed");
    assert_file_missing(session.output_path);

    recording_shutdown_transport();
    teardown_env(&env);
}

int main(void) {
    test_start_failure_cleans_up();
    test_successful_stop_preserves_output_and_stats();
    test_nonzero_exit_removes_partial_output();
    test_timeout_kills_hung_encoder();
    puts("recording_session_tests: ok");
    return 0;
}
