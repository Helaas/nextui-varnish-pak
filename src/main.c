/*
 * main.c — Varnish overlay service entry point.
 *
 * Usage:
 *   varnish              Enable startup wiring + start daemon (default)
 *   varnish --daemon     Start daemon (background)
 *   varnish --install    Enable startup wiring + start daemon
 *   varnish --uninstall  Disable startup wiring + stop daemon
 *   varnish --kill       Send SIGTERM to running daemon
 *   varnish --pill ...   Send a pill command to the daemon
 *   varnish --hide ...   Send a hide command to the daemon
 *   varnish --clear      Send a clear command to the daemon
 *   varnish --record-start   Start video recording
 *   varnish --record-stop    Stop video recording
 *   varnish --record-toggle  Toggle video recording
 *   varnish --ui         Open the management UI
 */

#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_WIDGETS_IMPLEMENTATION
#include "apostrophe_widgets.h"

#include "control.h"
#include "daemon.h"
#include "hooks.h"
#include "ipc.h"
#include "ui.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_status_summary(const varnish_status *status) {
    char summary[160];

    if (!status) return;
    control_format_status(status, summary, sizeof(summary));
    fprintf(stderr, "%s\n", summary);
}

static int cmd_install(const char *self_path) {
    varnish_status status;

    if (control_enable(self_path, &status) != 0) {
        fprintf(stderr, "varnish: could not fully enable startup wiring\n");
        print_status_summary(&status);
        return 1;
    }

    fprintf(stderr, "varnish: enabled successfully\n");
    print_status_summary(&status);
    fprintf(stderr, "varnish: reboot required for LD_PRELOAD to affect the current launcher session\n");
    return 0;
}

static int cmd_uninstall(void) {
    varnish_status status;

    if (control_disable(&status) != 0) {
        fprintf(stderr, "varnish: could not fully disable startup wiring\n");
        print_status_summary(&status);
        return 1;
    }

    fprintf(stderr, "varnish: disabled successfully\n");
    print_status_summary(&status);
    fprintf(stderr, "varnish: reboot required to fully unload the current launcher session\n");
    return 0;
}

static int cmd_kill(void) {
    if (!ipc_daemon_running()) {
        fprintf(stderr, "varnish: daemon is not running\n");
        return 1;
    }
    if (ipc_kill_daemon() < 0) {
        fprintf(stderr, "varnish: failed to kill daemon\n");
        return 1;
    }
    fprintf(stderr, "varnish: daemon stopped\n");
    return 0;
}

static int parse_duration_arg(const char *text, int *out) {
    char *end = NULL;
    long value;

    if (!text || !out)
        return -1;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value > INT_MAX) {
        return -1;
    }

    *out = (int)value;
    return 0;
}

static int cmd_pill(int argc, char *argv[]) {
    int duration_secs;

    if (argc != 6 || parse_duration_arg(argv[4], &duration_secs) != 0) {
        fprintf(stderr,
                "Usage: varnish --pill <client_id> <position> <duration_secs> <text>\n");
        return 1;
    }

    if (ipc_send_pill(argv[2], argv[3], duration_secs, argv[5]) != 0) {
        fprintf(stderr, "varnish: failed to send pill command\n");
        return 1;
    }

    return 0;
}

static int cmd_hide(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: varnish --hide <client_id>\n");
        return 1;
    }

    if (ipc_send_hide(argv[2]) != 0) {
        fprintf(stderr, "varnish: failed to send hide command\n");
        return 1;
    }

    return 0;
}

static int cmd_clear(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: varnish --clear\n");
        return 1;
    }

    if (ipc_send_clear() != 0) {
        fprintf(stderr, "varnish: failed to send clear command\n");
        return 1;
    }

    return 0;
}

static int cmd_record_start(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: varnish --record-start\n");
        return 1;
    }
    if (ipc_record_start() != 0) {
        fprintf(stderr, "varnish: failed to send record-start command\n");
        return 1;
    }
    return 0;
}

static int cmd_record_stop(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: varnish --record-stop\n");
        return 1;
    }
    if (ipc_record_stop() != 0) {
        fprintf(stderr, "varnish: failed to send record-stop command\n");
        return 1;
    }
    return 0;
}

static int cmd_record_toggle(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: varnish --record-toggle\n");
        return 1;
    }
    if (ipc_record_toggle() != 0) {
        fprintf(stderr, "varnish: failed to send record-toggle command\n");
        return 1;
    }
    return 0;
}

static const char *resolve_font_path(const char *self_path,
                                     char *buf, size_t buf_size) {
#ifdef PLATFORM_MAC
    char resolved[PATH_MAX];
    char *slash;

    if (access("third_party/apostrophe/res/font.ttf", R_OK) == 0)
        return "third_party/apostrophe/res/font.ttf";

    if (!self_path || !self_path[0]) return "font.ttf";
    if (!realpath(self_path, resolved)) return "font.ttf";

    slash = strrchr(resolved, '/');
    if (!slash) return "font.ttf";
    *slash = '\0';

    snprintf(buf, buf_size, "%s/font.ttf", resolved);
    return buf;
#else
    (void)self_path;
    (void)buf;
    (void)buf_size;
    return NULL;
#endif
}

static int cmd_ui(const char *self_path) {
    ap_config cfg = {0};
    char font_path[PATH_MAX];

    cfg.window_title = "Varnish";
    cfg.font_path = resolve_font_path(self_path, font_path, sizeof(font_path));
    cfg.log_path = ap_resolve_log_path("varnish");
    cfg.is_nextui = AP_PLATFORM_IS_DEVICE;
    cfg.cpu_speed = AP_CPU_SPEED_MENU;

    if (ap_init(&cfg) != AP_OK) {
        fprintf(stderr, "varnish: failed to initialise UI: %s\n",
                ap_get_error());
        return 1;
    }

    if (ui_run(self_path) != 0) {
        ap_quit();
        return 1;
    }

    ap_quit();
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "--daemon") == 0)
            return daemon_run();
        if (strcmp(argv[1], "--install") == 0)
            return cmd_install(argv[0]);
        if (strcmp(argv[1], "--uninstall") == 0)
            return cmd_uninstall();
        if (strcmp(argv[1], "--kill") == 0)
            return cmd_kill();
        if (strcmp(argv[1], "--pill") == 0)
            return cmd_pill(argc, argv);
        if (strcmp(argv[1], "--hide") == 0)
            return cmd_hide(argc, argv);
        if (strcmp(argv[1], "--clear") == 0)
            return cmd_clear(argc, argv);
        if (strcmp(argv[1], "--record-start") == 0)
            return cmd_record_start(argc, argv);
        if (strcmp(argv[1], "--record-stop") == 0)
            return cmd_record_stop(argc, argv);
        if (strcmp(argv[1], "--record-toggle") == 0)
            return cmd_record_toggle(argc, argv);
        if (strcmp(argv[1], "--ui") == 0)
            return cmd_ui(argv[0]);
        if (strcmp(argv[1], "--startup-env") == 0)
            return hooks_write_startup_env(stdout) == 0 ? 0 : 1;
        if (strcmp(argv[1], "--boot-hook") == 0)
            return hooks_boot_check(argv[0]);

        fprintf(stderr,
                "Usage: varnish [--daemon|--install|--uninstall|--kill|--pill|--hide|--clear|--record-start|--record-stop|--record-toggle|--ui]\n");
        return 1;
    }

    /* Default: enable startup wiring + start daemon */
    return cmd_install(argv[0]);
}
