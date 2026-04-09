/*
 * ipc.c — FIFO-based IPC for Varnish overlay daemon.
 *
 * The daemon listens on a named FIFO for overlay commands from client Paks.
 * Commands are newline-terminated text:
 *
 *   PILL <client_id> <position> <duration_secs> <text...>
 *   HIDE <client_id>
 *   CLEAR
 *   QUIT
 *   HOTKEYS_RELOAD
 *   HOTKEYS_PAUSE
 *   HOTKEYS_RESUME
 */

#include "ipc.h"
#include "strutil.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fifo_fd = -1;

/* Partial line buffer for handling reads that split across calls */
static char line_buf[1024];
static int  line_buf_len;

static int ipc_send_line(const char *line) {
    int fd;
    size_t len;
    ssize_t wrote;

    if (!line || !line[0])
        return -1;

    fd = open(VARNISH_FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    len = strlen(line);
    wrote = write(fd, line, len);
    close(fd);
    return wrote == (ssize_t)len ? 0 : -1;
}

/* ── Daemon side ───────────────────────────────────────────────── */

int ipc_init(void) {
    unlink(VARNISH_FIFO_PATH);
    if (mkfifo(VARNISH_FIFO_PATH, 0666) < 0 && errno != EEXIST) {
        perror("varnish: mkfifo");
        return -1;
    }

    fifo_fd = open(VARNISH_FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) {
        perror("varnish: open fifo (rd)");
        return -1;
    }

    /* Keep a write-end open so reads never return EOF when the last
       writer closes.  Intentionally leaked for daemon lifetime. */
    int wr = open(VARNISH_FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (wr < 0) {
        perror("varnish: open fifo (wr-keepalive)");
    }

    line_buf_len = 0;
    return 0;
}

static int parse_line(const char *line, ipc_cmd_t *cmd) {
    memset(cmd, 0, sizeof(*cmd));

    if (strcmp(line, "QUIT") == 0) {
        cmd->type = IPC_CMD_QUIT;
        return 1;
    }

    if (strcmp(line, "CLEAR") == 0) {
        cmd->type = IPC_CMD_CLEAR;
        return 1;
    }

    if (strcmp(line, "HOTKEYS_RELOAD") == 0) {
        cmd->type = IPC_CMD_HOTKEYS_RELOAD;
        return 1;
    }

    if (strcmp(line, "HOTKEYS_PAUSE") == 0) {
        cmd->type = IPC_CMD_HOTKEYS_PAUSE;
        return 1;
    }

    if (strcmp(line, "HOTKEYS_RESUME") == 0) {
        cmd->type = IPC_CMD_HOTKEYS_RESUME;
        return 1;
    }

    if (strncmp(line, "HIDE ", 5) == 0) {
        cmd->type = IPC_CMD_HIDE;
        str_copy_trunc(cmd->client_id, sizeof(cmd->client_id), line + 5);
        return 1;
    }

    if (strncmp(line, "PILL ", 5) == 0) {
        /* PILL <client_id> <position> <duration_secs> <text...> */
        const char *p = line + 5;
        char *end;

        /* client_id (no spaces allowed) */
        end = strchr(p, ' ');
        if (!end) return 0;
        {
            size_t len = (size_t)(end - p);
            if (len >= sizeof(cmd->client_id)) len = sizeof(cmd->client_id) - 1;
            memcpy(cmd->client_id, p, len);
            cmd->client_id[len] = '\0';
        }
        p = end + 1;

        /* position */
        end = strchr(p, ' ');
        if (!end) return 0;
        {
            size_t len = (size_t)(end - p);
            if (len >= sizeof(cmd->position)) len = sizeof(cmd->position) - 1;
            memcpy(cmd->position, p, len);
            cmd->position[len] = '\0';
        }
        p = end + 1;

        /* duration_secs */
        cmd->duration_secs = (int)strtol(p, &end, 10);
        if (end == p || (*end != ' ' && *end != '\0')) return 0;
        if (*end == ' ') end++;
        p = end;

        /* text (rest of line) */
        str_copy_trunc(cmd->text, sizeof(cmd->text), p);

        cmd->type = IPC_CMD_PILL;
        return 1;
    }

    return 0;
}

int ipc_read(ipc_cmd_t *cmd) {
    char chunk[512];
    ssize_t n;

    if (fifo_fd < 0 || !cmd) return 0;

    /* Read available data into the line buffer */
    while (line_buf_len < (int)sizeof(line_buf) - 1) {
        n = read(fifo_fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        /* Append to line buffer, clamping to available space */
        int space = (int)sizeof(line_buf) - 1 - line_buf_len;
        if (n > space) n = space;
        memcpy(line_buf + line_buf_len, chunk, (size_t)n);
        line_buf_len += (int)n;
    }
    line_buf[line_buf_len] = '\0';

    /* Find the first complete line */
    char *nl = strchr(line_buf, '\n');
    if (!nl) return 0;

    *nl = '\0';
    int parsed = parse_line(line_buf, cmd);

    /* Shift remaining data to front of buffer */
    int consumed = (int)(nl - line_buf) + 1;
    line_buf_len -= consumed;
    if (line_buf_len > 0)
        memmove(line_buf, nl + 1, (size_t)line_buf_len);
    line_buf[line_buf_len] = '\0';

    return parsed;
}

void ipc_write_pid(void) {
    FILE *f = fopen(VARNISH_PID_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
}

int ipc_daemon_running(void) {
    FILE *f = fopen(VARNISH_PID_PATH, "r");
    int pid = 0;

    if (!f) return 0;
    if (fscanf(f, "%d", &pid) != 1) { fclose(f); return 0; }
    fclose(f);
    if (pid <= 0) return 0;
    return (kill((pid_t)pid, 0) == 0) ? 1 : 0;
}

int ipc_kill_daemon(void) {
    FILE *f = fopen(VARNISH_PID_PATH, "r");
    int pid = 0;

    if (!f) return -1;
    if (fscanf(f, "%d", &pid) != 1) { fclose(f); return -1; }
    fclose(f);
    if (pid <= 0) return -1;
    return kill((pid_t)pid, SIGTERM);
}

int ipc_hotkeys_reload(void) {
    return ipc_send_line("HOTKEYS_RELOAD\n");
}

int ipc_hotkeys_pause(void) {
    return ipc_send_line("HOTKEYS_PAUSE\n");
}

int ipc_hotkeys_resume(void) {
    return ipc_send_line("HOTKEYS_RESUME\n");
}

void ipc_cleanup(void) {
    if (fifo_fd >= 0) { close(fifo_fd); fifo_fd = -1; }
    unlink(VARNISH_FIFO_PATH);
    unlink(VARNISH_PID_PATH);
}
