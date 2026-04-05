/*
 * daemon.h — Varnish overlay daemon.
 */

#ifndef VARNISH_DAEMON_H
#define VARNISH_DAEMON_H

int daemon_run(void);
int daemon_spawn_background(const char *self_path);

#endif /* VARNISH_DAEMON_H */
