/*
 * hooks.h — Boot hook and preload wrapper management for Varnish.
 */

#ifndef VARNISH_HOOKS_H
#define VARNISH_HOOKS_H

#include <stdbool.h>

int  hooks_install_preload(void);
int  hooks_uninstall_preload(void);
int  hooks_install_boot(void);
int  hooks_uninstall_boot(void);
bool hooks_preload_installed(void);
bool hooks_boot_installed(void);

#endif /* VARNISH_HOOKS_H */
