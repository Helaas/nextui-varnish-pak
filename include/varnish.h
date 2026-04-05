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
 */

#ifndef VARNISH_CLIENT_H
#define VARNISH_CLIENT_H

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define VARNISH_FIFO "/tmp/varnish.fifo"

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
    int fd = open(VARNISH_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    dprintf(fd, "PILL %s %s %d %s\n", client_id, position, duration_secs, text);
    close(fd);
    return 0;
}

/*
 * Hide the pill for the given client.
 * Returns 0 on success, -1 if daemon is not running.
 */
static inline int varnish_hide(const char *client_id) {
    int fd = open(VARNISH_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    dprintf(fd, "HIDE %s\n", client_id);
    close(fd);
    return 0;
}

/*
 * Clear all overlay pills from all clients.
 * Returns 0 on success, -1 if daemon is not running.
 */
static inline int varnish_clear(void) {
    int fd = open(VARNISH_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    dprintf(fd, "CLEAR\n");
    close(fd);
    return 0;
}

#endif /* VARNISH_CLIENT_H */
