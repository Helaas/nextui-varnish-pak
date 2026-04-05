/*
 * main.c — Varnish overlay service entry point.
 *
 * Usage:
 *   varnish              Install hooks + start daemon (default)
 *   varnish --daemon     Start daemon (background)
 *   varnish --install    Install hooks only
 *   varnish --uninstall  Uninstall hooks, restore nextui.elf
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int cmd_install(void) {
    int err = 0;

    if (hooks_install_preload() < 0) {
        fprintf(stderr, "varnish: preload wrapper install failed\n");
        err++;
    }
    if (hooks_install_boot() < 0) {
        fprintf(stderr, "varnish: boot hook install failed\n");
        err++;
    }

    if (!err)
        fprintf(stderr, "varnish: hooks installed successfully\n");
    return err ? 1 : 0;
}

static int cmd_uninstall(void) {
    int err = 0;

    /* Kill daemon first if running */
    if (ipc_daemon_running()) {
        fprintf(stderr, "varnish: stopping daemon...\n");
        ipc_kill_daemon();
        /* Give it a moment to clean up */
        usleep(500000);
    }

    if (hooks_uninstall_preload() < 0) {
        fprintf(stderr, "varnish: preload wrapper uninstall failed\n");
        err++;
    }
    if (hooks_uninstall_boot() < 0) {
        fprintf(stderr, "varnish: boot hook uninstall failed\n");
        err++;
    }

    if (!err)
        fprintf(stderr, "varnish: hooks uninstalled successfully\n");
    return err ? 1 : 0;
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
            return cmd_install();
        if (strcmp(argv[1], "--uninstall") == 0)
            return cmd_uninstall();
        if (strcmp(argv[1], "--kill") == 0)
            return cmd_kill();
        if (strcmp(argv[1], "--ui") == 0)
            return cmd_ui(argv[0]);

        fprintf(stderr,
                "Usage: varnish [--daemon|--install|--uninstall|--kill|--ui]\n");
        return 1;
    }

    /* Default: install hooks + start daemon */
    cmd_install();
    return daemon_run();
}
