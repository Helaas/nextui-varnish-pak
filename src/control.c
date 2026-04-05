/*
 * control.c — Varnish UI control and live status helpers.
 */

#include "control.h"

#include "daemon.h"
#include "hooks.h"
#include "ipc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CONTROL_POLL_MS            50
#define CONTROL_START_TIMEOUT_MS 1500
#define CONTROL_STOP_TIMEOUT_MS  2000

static bool wait_for_daemon_state(bool running, int timeout_ms) {
    int waited_ms = 0;

    while (waited_ms <= timeout_ms) {
        if ((ipc_daemon_running() != 0) == running)
            return true;
        usleep(CONTROL_POLL_MS * 1000);
        waited_ms += CONTROL_POLL_MS;
    }

    return (ipc_daemon_running() != 0) == running;
}

void control_get_status(varnish_status *out) {
    if (!out) return;

    memset(out, 0, sizeof(*out));
    out->enabled = hooks_is_enabled();
    out->startup_installed = hooks_startup_installed();
    out->boot_installed = hooks_boot_installed();
    out->daemon_running = ipc_daemon_running();
}

bool control_is_enabled(const varnish_status *status) {
    return status && status->enabled;
}

void control_format_status(const varnish_status *status, char *out, size_t size) {
    if (!out || size == 0) return;
    out[0] = '\0';
    if (!status) return;

    snprintf(out, size,
             "Startup patch: %s\n"
             "Boot hook: %s\n"
             "Daemon: %s",
             status->startup_installed ? "Installed" : "Missing",
             status->boot_installed ? "Installed" : "Missing",
             status->daemon_running ? "Running" : "Stopped");
}

int control_enable(const char *self_path, varnish_status *out_status) {
    int err = 0;

    if (hooks_set_enabled(true) < 0)
        err++;
    if (hooks_install_startup() < 0)
        err++;
    if (hooks_install_boot() < 0)
        err++;

    if (!ipc_daemon_running()) {
        if (daemon_spawn_background(self_path) < 0)
            err++;
        else if (!wait_for_daemon_state(true, CONTROL_START_TIMEOUT_MS))
            err++;
    }

    control_get_status(out_status);

    if (out_status) {
        if (!out_status->enabled)
            err++;
        if (!out_status->startup_installed)
            err++;
        if (!out_status->boot_installed)
            err++;
        if (!out_status->daemon_running)
            err++;
    }

    return err ? -1 : 0;
}

int control_disable(varnish_status *out_status) {
    int err = 0;

    if (hooks_set_enabled(false) < 0)
        err++;
    if (hooks_uninstall_startup() < 0)
        err++;
    if (hooks_uninstall_boot() < 0)
        err++;

    if (ipc_daemon_running()) {
        if (ipc_kill_daemon() < 0)
            err++;
        else if (!wait_for_daemon_state(false, CONTROL_STOP_TIMEOUT_MS))
            err++;
    }

    control_get_status(out_status);

    if (out_status) {
        if (out_status->enabled)
            err++;
        if (out_status->startup_installed)
            err++;
        if (out_status->boot_installed)
            err++;
        if (out_status->daemon_running)
            err++;
    }

    return err ? -1 : 0;
}
