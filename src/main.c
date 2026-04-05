/*
 * main.c — Varnish overlay service entry point.
 *
 * Usage:
 *   varnish              Install hooks + start daemon (default)
 *   varnish --daemon     Start daemon (background)
 *   varnish --install    Install hooks only
 *   varnish --uninstall  Uninstall hooks, restore nextui.elf
 *   varnish --kill       Send SIGTERM to running daemon
 */

#include "daemon.h"
#include "hooks.h"
#include "ipc.h"

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

        fprintf(stderr, "Usage: varnish [--daemon|--install|--uninstall|--kill]\n");
        return 1;
    }

    /* Default: install hooks + start daemon */
    cmd_install();
    return daemon_run();
}
