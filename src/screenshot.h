/*
 * screenshot.h — Framebuffer screenshot helpers.
 */

#ifndef VARNISH_SCREENSHOT_H
#define VARNISH_SCREENSHOT_H

#include <stddef.h>
#include <time.h>

int screenshot_resolve_output_path(const char *dir, time_t now,
                                   char *out_path, size_t out_size);
int screenshot_capture(char *out_path, size_t out_size);

#endif /* VARNISH_SCREENSHOT_H */
