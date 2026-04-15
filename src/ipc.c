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
 *   RECORD_START
 *   RECORD_STOP
 *   RECORD_TOGGLE
 */

#include "ipc.h"
#include "strutil.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fifo_fd = -1;
static int fifo_wr_fd = -1;

/* Partial line buffer for handling reads that split across calls */
static char line_buf[1024];
static int  line_buf_len;
static int  discarding_line;

static int ipc_valid_client_id(const char *client_id) {
    const unsigned char *p = (const unsigned char *)client_id;

    if (!client_id || !client_id[0])
        return 0;

    while (*p) {
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
            return 0;
        p++;
    }

    return 1;
}

static int ipc_valid_position(const char *position) {
    return position &&
           (strcmp(position, "top-left") == 0 ||
            strcmp(position, "top-center") == 0 ||
            strcmp(position, "top-right") == 0 ||
            strcmp(position, "bottom-left") == 0 ||
            strcmp(position, "bottom-center") == 0 ||
            strcmp(position, "bottom-right") == 0);
}

static void ipc_sanitize_text(const char *src, char *dst, size_t dst_size) {
    size_t i = 0;

    if (!dst || dst_size == 0)
        return;

    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (*src && i + 1 < dst_size) {
        char ch = *src++;
        if (ch == '\n' || ch == '\r')
            ch = ' ';
        dst[i++] = ch;
    }

    dst[i] = '\0';
}

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

int ipc_send_pill(const char *client_id, const char *position,
                  int duration_secs, const char *text) {
    char sanitized_text[256];
    char line[384];
    int line_len;

    if (!ipc_valid_client_id(client_id) ||
        !ipc_valid_position(position) ||
        duration_secs < 0 || !text) {
        return -1;
    }

    ipc_sanitize_text(text, sanitized_text, sizeof(sanitized_text));
    line_len = snprintf(line, sizeof(line), "PILL %s %s %d %s\n",
                        client_id, position, duration_secs, sanitized_text);
    if (line_len < 0 || line_len >= (int)sizeof(line))
        return -1;

    return ipc_send_line(line);
}

int ipc_send_hide(const char *client_id) {
    char line[96];
    int line_len;

    if (!ipc_valid_client_id(client_id))
        return -1;

    line_len = snprintf(line, sizeof(line), "HIDE %s\n", client_id);
    if (line_len < 0 || line_len >= (int)sizeof(line))
        return -1;

    return ipc_send_line(line);
}

int ipc_send_clear(void) {
    return ipc_send_line("CLEAR\n");
}

/* ── Daemon side ───────────────────────────────────────────────── */

int ipc_init(void) {
    unlink(VARNISH_FIFO_PATH);
    if (mkfifo(VARNISH_FIFO_PATH, 0600) < 0 && errno != EEXIST) {
        perror("varnish: mkfifo");
        return -1;
    }

    fifo_fd = open(VARNISH_FIFO_PATH, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fifo_fd < 0) {
        perror("varnish: open fifo (rd)");
        return -1;
    }

    /* Keep a write-end open so reads never return EOF when the last
       writer closes.  Closed in ipc_cleanup(). */
    fifo_wr_fd = open(VARNISH_FIFO_PATH, O_WRONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fifo_wr_fd < 0) {
        perror("varnish: open fifo (wr-keepalive)");
        close(fifo_fd);
        fifo_fd = -1;
        return -1;
    }

    line_buf_len = 0;
    discarding_line = 0;
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

    if (strcmp(line, "RECORD_START") == 0) {
        cmd->type = IPC_CMD_RECORD_START;
        return 1;
    }

    if (strcmp(line, "RECORD_STOP") == 0) {
        cmd->type = IPC_CMD_RECORD_STOP;
        return 1;
    }

    if (strcmp(line, "RECORD_TOGGLE") == 0) {
        cmd->type = IPC_CMD_RECORD_TOGGLE;
        return 1;
    }

    if (strncmp(line, "HIDE ", 5) == 0) {
        if (!ipc_valid_client_id(line + 5))
            return 0;
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
        if (!ipc_valid_client_id(cmd->client_id))
            return 0;
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
        if (!ipc_valid_position(cmd->position))
            return 0;
        p = end + 1;

        /* duration_secs */
        cmd->duration_secs = (int)strtol(p, &end, 10);
        if (end == p || (*end != ' ' && *end != '\0')) return 0;
        if (cmd->duration_secs < 0) return 0;
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
    char ch;
    ssize_t n;
    int parsed;

    if (fifo_fd < 0 || !cmd) return 0;

    while ((n = read(fifo_fd, &ch, 1)) > 0) {
        if (discarding_line) {
            if (ch == '\n')
                discarding_line = 0;
            continue;
        }

        if (ch == '\n') {
            line_buf[line_buf_len] = '\0';
            parsed = parse_line(line_buf, cmd);
            line_buf_len = 0;
            if (parsed)
                return 1;
            continue;
        }

        if (line_buf_len >= (int)sizeof(line_buf) - 1) {
            line_buf_len = 0;
            line_buf[0] = '\0';
            discarding_line = 1;
            continue;
        }

        line_buf[line_buf_len++] = ch;
    }

    return 0;
}

void ipc_write_pid(void) {
    FILE *f = fopen(VARNISH_PID_PATH, "w");
    if (!f) {
        perror("varnish: cannot create PID file");
        return;
    }
    if (fprintf(f, "%d\n", (int)getpid()) < 0)
        perror("varnish: cannot write PID file");
    if (fclose(f) != 0)
        perror("varnish: cannot close PID file");
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

int ipc_record_start(void) {
    return ipc_send_line("RECORD_START\n");
}

int ipc_record_stop(void) {
    return ipc_send_line("RECORD_STOP\n");
}

int ipc_record_toggle(void) {
    return ipc_send_line("RECORD_TOGGLE\n");
}

void ipc_cleanup(void) {
    if (fifo_wr_fd >= 0) { close(fifo_wr_fd); fifo_wr_fd = -1; }
    if (fifo_fd >= 0) { close(fifo_fd); fifo_fd = -1; }
    unlink(VARNISH_FIFO_PATH);
    unlink(VARNISH_PID_PATH);
}
