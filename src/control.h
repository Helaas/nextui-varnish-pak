/*
 * control.h — Varnish UI control and live status helpers.
 */

#ifndef VARNISH_CONTROL_H
#define VARNISH_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool enabled;
    bool startup_installed;
    bool boot_installed;
    bool daemon_running;
} varnish_status;

void control_get_status(varnish_status *out);
bool control_is_enabled(const varnish_status *status);
void control_format_status(const varnish_status *status, char *out, size_t size);
int  control_enable(const char *self_path, varnish_status *out_status);
int  control_disable(varnish_status *out_status);
int  control_request_reboot(void);

#endif /* VARNISH_CONTROL_H */
