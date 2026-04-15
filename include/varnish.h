/*
 * varnish.h — Header-only client library for Varnish overlay system.
 *
 * Include this header in your Pak to show on-screen pill notifications
 * via the Varnish daemon.  No linking required — just include and call.
 *
 * Requires: <fcntl.h>, <stdio.h>, <string.h>, <unistd.h>
 *
 * Example:
 *   #include <varnish.h>
 *
 *   // Show a pill at bottom-center for 5 seconds
 *   varnish_pill("mypak", "bottom-center", 5, "Download complete!");
 *
 *   // Show an indefinite pill (stays until hidden)
 *   varnish_pill("mypak", "top-right", 0, "Syncing...");
 *
 *   // Hide it
 *   varnish_hide("mypak");
 *
 * Positions: top-left, top-center, top-right,
 *            bottom-left, bottom-center, bottom-right
 *
 * If the Varnish daemon is not running, calls fail silently (return -1).
 * Invalid client IDs / positions also return -1. Newlines in text are
 * normalized to spaces before the command is written to the FIFO.
 */

#ifndef VARNISH_CLIENT_H
#define VARNISH_CLIENT_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef VARNISH_FIFO
#define VARNISH_FIFO "/tmp/varnish.fifo"
#endif

static inline int varnish_valid_client_id(const char *client_id) {
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

static inline int varnish_valid_position(const char *position) {
    return position &&
           (strcmp(position, "top-left") == 0 ||
            strcmp(position, "top-center") == 0 ||
            strcmp(position, "top-right") == 0 ||
            strcmp(position, "bottom-left") == 0 ||
            strcmp(position, "bottom-center") == 0 ||
            strcmp(position, "bottom-right") == 0);
}

static inline void varnish_sanitize_text(const char *src, char *dst, size_t dst_size) {
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

static inline int varnish_send_line(const char *line) {
    int fd;
    size_t len;
    ssize_t wrote;

    if (!line || !line[0])
        return -1;

    fd = open(VARNISH_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    len = strlen(line);
    do {
        wrote = write(fd, line, len);
    } while (wrote < 0 && errno == EINTR);
    close(fd);
    return wrote == (ssize_t)len ? 0 : -1;
}

/*
 * Show a pill notification.
 *
 * client_id:      Short identifier for your Pak (e.g., "menulody").
 *                 Each client_id gets one slot — calling again with the
 *                 same client_id replaces the previous pill.
 * position:       One of: "top-left", "top-center", "top-right",
 *                         "bottom-left", "bottom-center", "bottom-right"
 * duration_secs:  How long to show (seconds). 0 = indefinite (until varnish_hide).
 * text:           The text to display in the pill.
 *
 * Returns 0 on success, -1 if daemon is not running.
 */
static inline int varnish_pill(const char *client_id, const char *position,
                               int duration_secs, const char *text) {
    char sanitized_text[256];
    char line[384];
    int line_len;

    if (!varnish_valid_client_id(client_id) ||
        !varnish_valid_position(position) ||
        duration_secs < 0 || !text) {
        return -1;
    }

    varnish_sanitize_text(text, sanitized_text, sizeof(sanitized_text));
    line_len = snprintf(line, sizeof(line), "PILL %s %s %d %s\n",
                        client_id, position, duration_secs, sanitized_text);
    if (line_len < 0 || line_len >= (int)sizeof(line))
        return -1;

    return varnish_send_line(line);
}

/*
 * Hide the pill for the given client.
 * Returns 0 on success, -1 if daemon is not running.
 */
static inline int varnish_hide(const char *client_id) {
    char line[96];
    int line_len;

    if (!varnish_valid_client_id(client_id))
        return -1;

    line_len = snprintf(line, sizeof(line), "HIDE %s\n", client_id);
    if (line_len < 0 || line_len >= (int)sizeof(line))
        return -1;

    return varnish_send_line(line);
}

/*
 * Clear all overlay pills from all clients.
 * Returns 0 on success, -1 if daemon is not running.
 */
static inline int varnish_clear(void) {
    return varnish_send_line("CLEAR\n");
}

#endif /* VARNISH_CLIENT_H */
