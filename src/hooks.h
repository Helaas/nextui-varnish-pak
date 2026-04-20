/*
 * hooks.h — Enabled marker, startup patch, and boot hook management.
 */

#ifndef VARNISH_HOOKS_H
#define VARNISH_HOOKS_H

#include <stdbool.h>
#include <stdio.h>

int  hooks_set_enabled(bool enabled);
bool hooks_is_enabled(void);

int  hooks_install_startup(void);
int  hooks_uninstall_startup(void);
bool hooks_startup_installed(void);

int  hooks_install_boot(void);
int  hooks_uninstall_boot(void);
bool hooks_boot_installed(void);

int  hooks_write_startup_env(FILE *out);
int  hooks_boot_check(const char *self_path);

#endif /* VARNISH_HOOKS_H */
