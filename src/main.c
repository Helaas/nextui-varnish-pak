/*
 * main.c — Varnish overlay service entry point.
 *
 * Usage:
 *   varnish              Enable startup wiring + start daemon (default)
 *   varnish --daemon     Start daemon (background)
 *   varnish --install    Enable startup wiring + start daemon
 *   varnish --uninstall  Disable startup wiring + stop daemon
 *   varnish --kill       Send SIGTERM to running daemon
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
        if (strcmp(argv[1], "--ui") == 0)
            return cmd_ui(argv[0]);
        if (strcmp(argv[1], "--startup-env") == 0)
            return hooks_write_startup_env(stdout) == 0 ? 0 : 1;
        if (strcmp(argv[1], "--boot-hook") == 0)
            return hooks_boot_check(argv[0]);

        fprintf(stderr,
                "Usage: varnish [--daemon|--install|--uninstall|--kill|--ui]\n");
        return 1;
    }

    /* Default: enable startup wiring + start daemon */
    return cmd_install(argv[0]);
}
