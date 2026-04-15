/*
 * ipc_tests.c — Host-side tests for FIFO IPC parsing and client helpers.
 */

#include "ipc.h"
#include "varnish.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

#define CHECK(cond, msg) do { \
    if (!(cond)) fail(msg); \
} while (0)

static void write_all_or_fail(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t wrote = write(fd, buf, len);
        CHECK(wrote >= 0, "write failed");
        buf += (size_t)wrote;
        len -= (size_t)wrote;
    }
}

static int wait_for_cmd(ipc_cmd_t *cmd) {
    for (int i = 0; i < 200; i++) {
        if (ipc_read(cmd))
            return 1;
        usleep(1000);
    }
    return 0;
}

static void test_header_client_sanitizes_text(void) {
    ipc_cmd_t cmd;

    CHECK(varnish_pill("mypak", "top-right", 5, "line1\nline2\rline3") == 0,
          "header helper should send pill");
    CHECK(wait_for_cmd(&cmd), "expected pill command");
    CHECK(cmd.type == IPC_CMD_PILL, "expected PILL command type");
    CHECK(strcmp(cmd.client_id, "mypak") == 0, "unexpected client_id");
    CHECK(strcmp(cmd.position, "top-right") == 0, "unexpected position");
    CHECK(cmd.duration_secs == 5, "unexpected duration");
    CHECK(strcmp(cmd.text, "line1 line2 line3") == 0,
          "text should replace CR/LF with spaces");
}

static void test_invalid_client_inputs_are_rejected(void) {
    CHECK(varnish_pill("bad id", "top-right", 5, "hello") == -1,
          "header helper should reject invalid client IDs");
    CHECK(varnish_pill("mypak", "middle", 5, "hello") == -1,
          "header helper should reject invalid positions");
    CHECK(varnish_hide("bad id") == -1,
          "hide helper should reject invalid client IDs");
    CHECK(ipc_send_pill("bad id", "top-right", 0, "hello") == -1,
          "ipc_send_pill should reject invalid client IDs");
}

static void test_ipc_send_helpers(void) {
    ipc_cmd_t cmd;

    CHECK(ipc_send_pill("shellpak", "bottom-center", 0, "hello") == 0,
          "ipc_send_pill should succeed");
    CHECK(wait_for_cmd(&cmd), "expected ipc_send_pill command");
    CHECK(cmd.type == IPC_CMD_PILL, "expected PILL command type from ipc_send_pill");
    CHECK(strcmp(cmd.client_id, "shellpak") == 0, "unexpected ipc_send_pill client_id");
    CHECK(strcmp(cmd.position, "bottom-center") == 0, "unexpected ipc_send_pill position");
    CHECK(cmd.duration_secs == 0, "unexpected ipc_send_pill duration");
    CHECK(strcmp(cmd.text, "hello") == 0, "unexpected ipc_send_pill text");

    CHECK(ipc_send_hide("shellpak") == 0, "ipc_send_hide should succeed");
    CHECK(wait_for_cmd(&cmd), "expected ipc_send_hide command");
    CHECK(cmd.type == IPC_CMD_HIDE, "expected HIDE command type");
    CHECK(strcmp(cmd.client_id, "shellpak") == 0, "unexpected hide client_id");

    CHECK(ipc_send_clear() == 0, "ipc_send_clear should succeed");
    CHECK(wait_for_cmd(&cmd), "expected CLEAR command");
    CHECK(cmd.type == IPC_CMD_CLEAR, "expected CLEAR command type");

    CHECK(ipc_record_start() == 0, "ipc_record_start should succeed");
    CHECK(wait_for_cmd(&cmd), "expected RECORD_START command");
    CHECK(cmd.type == IPC_CMD_RECORD_START, "expected RECORD_START command type");

    CHECK(ipc_record_stop() == 0, "ipc_record_stop should succeed");
    CHECK(wait_for_cmd(&cmd), "expected RECORD_STOP command");
    CHECK(cmd.type == IPC_CMD_RECORD_STOP, "expected RECORD_STOP command type");

    CHECK(ipc_record_toggle() == 0, "ipc_record_toggle should succeed");
    CHECK(wait_for_cmd(&cmd), "expected RECORD_TOGGLE command");
    CHECK(cmd.type == IPC_CMD_RECORD_TOGGLE, "expected RECORD_TOGGLE command type");
}

static void test_overlong_line_is_discarded(void) {
    char oversized[1400];
    ipc_cmd_t cmd;
    int fd;

    memset(oversized, 'A', sizeof(oversized));
    oversized[sizeof(oversized) - 2] = '\n';
    oversized[sizeof(oversized) - 1] = '\0';

    fd = open(VARNISH_FIFO_PATH, O_WRONLY | O_NONBLOCK);
    CHECK(fd >= 0, "could not open FIFO writer");
    write_all_or_fail(fd, oversized, strlen(oversized));
    write_all_or_fail(fd, "CLEAR\n", strlen("CLEAR\n"));
    close(fd);

    CHECK(wait_for_cmd(&cmd), "expected command after oversized line");
    CHECK(cmd.type == IPC_CMD_CLEAR,
          "oversized line should be discarded so CLEAR still parses");
}

int main(void) {
    CHECK(ipc_init() == 0, "ipc_init failed");

    test_header_client_sanitizes_text();
    test_invalid_client_inputs_are_rejected();
    test_ipc_send_helpers();
    test_overlong_line_is_discarded();

    ipc_cleanup();
    puts("ipc_tests: ok");
    return 0;
}
