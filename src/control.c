/*
 * control.c — Varnish UI control and live status helpers.
 */

#include "control.h"
#include "hooks.h"
#include "ipc.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
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

static int spawn_daemon(const char *self_path) {
    pid_t pid;
    int status;

    if (!self_path || !self_path[0]) return -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        pid_t grandchild = fork();
        char *const argv[] = { (char *)self_path, "--daemon", NULL };

        if (grandchild < 0) _exit(127);
        if (grandchild > 0) _exit(0);

        setsid();
        execvp(self_path, argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;

    return 0;
}

void control_get_status(varnish_status *out) {
    if (!out) return;

    memset(out, 0, sizeof(*out));
#ifdef PLATFORM_MAC
    out->preload_supported = false;
#else
    out->preload_supported = true;
#endif
    out->preload_installed = hooks_preload_installed();
    out->boot_installed = hooks_boot_installed();
    out->daemon_running = ipc_daemon_running();
}

bool control_is_enabled(const varnish_status *status) {
    return status && status->boot_installed;
}

void control_format_status(const varnish_status *status, char *out, size_t size) {
    const char *preload_state;

    if (!out || size == 0) return;
    out[0] = '\0';
    if (!status) return;

    if (status->preload_supported)
        preload_state = status->preload_installed ? "Installed" : "Missing";
    else
        preload_state = "Skipped (desktop)";

    snprintf(out, size,
             "Preload hook: %s\n"
             "Boot hook: %s\n"
             "Daemon: %s",
             preload_state,
             status->boot_installed ? "Enabled" : "Disabled",
             status->daemon_running ? "Running" : "Stopped");
}

int control_enable(const char *self_path, varnish_status *out_status) {
    int err = 0;

    if (hooks_install_preload() < 0 && hooks_preload_installed() == 0)
        err++;
    if (hooks_install_boot() < 0)
        err++;

    if (!ipc_daemon_running()) {
        if (spawn_daemon(self_path) < 0)
            err++;
        else if (!wait_for_daemon_state(true, CONTROL_START_TIMEOUT_MS))
            err++;
    }

    control_get_status(out_status);

    if (out_status) {
        if (out_status->preload_supported && !out_status->preload_installed)
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
        if (out_status->boot_installed)
            err++;
        if (out_status->daemon_running)
            err++;
    }

    return err ? -1 : 0;
}
