/*
 * daemon.c — Varnish overlay daemon.
 *
 * Manages overlay slots, listens for IPC commands from client Paks, renders
 * pills, and publishes them to shared memory for the preload hook to composite.
 */

#include "daemon.h"
#include "device.h"
#include "hooks.h"
#include "ipc.h"
#include "overlay.h"
#include "shm.h"
#include "strutil.h"
#include "varnish_shm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/stat.h>
#include <fcntl.h>
#endif

/* ── Slot management ───────────────────────────────────────────── */

typedef struct {
    int    in_use;
    char   client_id[64];
    time_t expire_at;     /* 0 = indefinite (until HIDE) */
    int    z_order;
} slot_state_t;

static slot_state_t slots[VARNISH_MAX_SLOTS];
static volatile sig_atomic_t quit_flag;

/* ── Warmup (delay publishing during early boot) ───────────────── */

#define WARMUP_SECS 5

static int warmup_active;
static struct timespec warmup_deadline;

static void warmup_start(void) {
    if (clock_gettime(CLOCK_MONOTONIC, &warmup_deadline) == 0) {
        warmup_deadline.tv_sec += WARMUP_SECS;
        warmup_active = 1;
        fprintf(stderr, "varnish: warmup started (%d seconds)\n", WARMUP_SECS);
    }
}

static int warmup_elapsed(void) {
    struct timespec now;
    if (!warmup_active) return 1;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 1;
    if (now.tv_sec > warmup_deadline.tv_sec) return 1;
    if (now.tv_sec == warmup_deadline.tv_sec &&
        now.tv_nsec >= warmup_deadline.tv_nsec) return 1;
    return 0;
}

static void warmup_update(void) {
    if (!warmup_active) return;
    if (warmup_elapsed()) {
        warmup_active = 0;
        fprintf(stderr, "varnish: warmup ended\n");
    }
}

/* ── Slot helpers ──────────────────────────────────────────────── */

static int slot_find_by_client(const char *client_id) {
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++) {
        if (slots[i].in_use && strcmp(slots[i].client_id, client_id) == 0)
            return i;
    }
    return -1;
}

static int slot_alloc(const char *client_id) {
    int idx;

    /* Reuse existing slot for this client */
    idx = slot_find_by_client(client_id);
    if (idx >= 0) return idx;

    /* Find first free slot */
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++) {
        if (!slots[i].in_use) {
            slots[i].in_use = 1;
            str_copy_trunc(slots[i].client_id, sizeof(slots[i].client_id), client_id);
            slots[i].z_order = i;
            return i;
        }
    }

    /* All slots full — evict the oldest expired slot */
    {
        time_t now = time(NULL);
        int oldest_idx = -1;
        time_t oldest_expire = 0;

        for (int i = 0; i < VARNISH_MAX_SLOTS; i++) {
            if (slots[i].expire_at > 0 && slots[i].expire_at <= now) {
                if (oldest_idx < 0 || slots[i].expire_at < oldest_expire) {
                    oldest_idx = i;
                    oldest_expire = slots[i].expire_at;
                }
            }
        }

        if (oldest_idx >= 0) {
            shm_slot_clear(oldest_idx);
            memset(&slots[oldest_idx], 0, sizeof(slots[oldest_idx]));
            slots[oldest_idx].in_use = 1;
            str_copy_trunc(slots[oldest_idx].client_id,
                           sizeof(slots[oldest_idx].client_id), client_id);
            slots[oldest_idx].z_order = oldest_idx;
            return oldest_idx;
        }
    }

    fprintf(stderr, "varnish: all %d slots in use, cannot allocate for %s\n",
            VARNISH_MAX_SLOTS, client_id);
    return -1;
}

static void slot_free(int idx) {
    if (idx < 0 || idx >= VARNISH_MAX_SLOTS) return;
    shm_slot_clear(idx);
    memset(&slots[idx], 0, sizeof(slots[idx]));
}

static void slot_expire_tick(void) {
    time_t now = time(NULL);
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++) {
        if (slots[i].in_use && slots[i].expire_at > 0 && now >= slots[i].expire_at) {
            fprintf(stderr, "varnish: slot %d (%s) expired\n", i, slots[i].client_id);
            slot_free(i);
        }
    }
}

static void slot_clear_all(void) {
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++) {
        if (slots[i].in_use)
            slot_free(i);
    }
}

/* ── Pill pixel buffer (shared across renders to avoid per-call allocation) ── */

static uint32_t pill_pixels[VARNISH_SLOT_MAX_W * VARNISH_SLOT_MAX_H];

/* ── Command handlers ──────────────────────────────────────────── */

static void handle_pill(const ipc_cmd_t *cmd) {
    int pos;
    int idx;
    int x, y, w, h;

    if (warmup_active) return;

    pos = overlay_parse_position(cmd->position);
    if (pos < 0) {
        fprintf(stderr, "varnish: unknown position '%s', defaulting to bottom-center\n",
                cmd->position);
        pos = VARNISH_POS_BOTTOM_CENTER;
    }

    idx = slot_alloc(cmd->client_id);
    if (idx < 0) return;

    if (overlay_render_pill(cmd->text, (varnish_position_t)pos,
                            &x, &y, &w, &h, pill_pixels) != 0) {
        fprintf(stderr, "varnish: render failed for '%s'\n", cmd->text);
        return;
    }

    slots[idx].z_order = idx;
    slots[idx].expire_at = (cmd->duration_secs > 0)
        ? time(NULL) + cmd->duration_secs
        : 0;

    shm_slot_update(idx, x, y, w, h, slots[idx].z_order, pill_pixels);

    fprintf(stderr, "varnish: slot %d (%s) -> \"%s\" at %dx%d+%d+%d dur=%ds\n",
            idx, cmd->client_id, cmd->text, w, h, x, y, cmd->duration_secs);
}

static void handle_hide(const ipc_cmd_t *cmd) {
    int idx = slot_find_by_client(cmd->client_id);
    if (idx >= 0) {
        fprintf(stderr, "varnish: hiding slot %d (%s)\n", idx, cmd->client_id);
        slot_free(idx);
    }
}

/* ── Signal handler ────────────────────────────────────────────── */

static void signal_handler(int sig) {
    (void)sig;
    quit_flag = 1;
}

/* ── Daemonize ─────────────────────────────────────────────────── */

static void daemonize(void) {
#ifdef __linux__
    pid_t pid = fork();
    if (pid < 0) {
        perror("varnish: fork");
        exit(1);
    }
    if (pid > 0) {
        /* Parent exits */
        _exit(0);
    }

    /* Child becomes session leader */
    setsid();

    /* Redirect stdio */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        /* Keep stderr for logging */
        close(devnull);
    }
#endif
}

/* ── Main daemon entry point ───────────────────────────────────── */

int daemon_run(void) {
    int fb_width = 0, fb_height = 0;
    ipc_cmd_t cmd;

    /* Daemonize (fork to background) */
    daemonize();

    /* Install signal handlers */
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Write PID file */
    ipc_write_pid();

    /* Detect framebuffer dimensions */
    if (device_get_fb_dimensions(&fb_width, &fb_height) < 0) {
        fprintf(stderr, "varnish: cannot detect framebuffer, using defaults\n");
        fb_width = 1024;
        fb_height = 768;
    }

    /* Initialize subsystems */
    if (shm_init(fb_width, fb_height) < 0) {
        fprintf(stderr, "varnish: shm init failed, exiting\n");
        ipc_cleanup();
        return 1;
    }

    if (ipc_init() < 0) {
        fprintf(stderr, "varnish: ipc init failed, exiting\n");
        shm_cleanup();
        return 1;
    }

    overlay_init(fb_width, fb_height);

    /* Start warmup timer */
    warmup_start();

    memset(slots, 0, sizeof(slots));

    fprintf(stderr, "varnish: daemon started (pid %d, fb %dx%d)\n",
            (int)getpid(), fb_width, fb_height);

    /* ── Main loop ─────────────────────────────────────────────── */

    while (!quit_flag) {
        /* Drain all pending IPC commands */
        while (ipc_read(&cmd)) {
            switch (cmd.type) {
            case IPC_CMD_PILL:
                handle_pill(&cmd);
                break;
            case IPC_CMD_HIDE:
                handle_hide(&cmd);
                break;
            case IPC_CMD_CLEAR:
                slot_clear_all();
                fprintf(stderr, "varnish: all slots cleared\n");
                break;
            case IPC_CMD_QUIT:
                quit_flag = 1;
                break;
            default:
                break;
            }
        }

        /* Expire timed-out slots */
        warmup_update();
        slot_expire_tick();

        /* ~50ms tick (20 Hz) */
        usleep(50000);
    }

    /* ── Cleanup ───────────────────────────────────────────────── */

    fprintf(stderr, "varnish: daemon shutting down\n");
    slot_clear_all();
    overlay_cleanup();
    shm_cleanup();
    ipc_cleanup();

    return 0;
}
