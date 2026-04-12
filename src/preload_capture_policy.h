#ifndef VARNISH_PRELOAD_CAPTURE_POLICY_H
#define VARNISH_PRELOAD_CAPTURE_POLICY_H

#include <stdint.h>

static inline int varnish_should_capture_background(
    int save_valid,
    const void *saved_renderer,
    int saved_x,
    int saved_y,
    int saved_w,
    int saved_h,
    uint32_t last_capture_ms,
    const void *renderer,
    int x,
    int y,
    int w,
    int h,
    uint32_t now_ms,
    uint32_t min_capture_ms)
{
    if (!save_valid)
        return 1;
    if (saved_renderer != renderer)
        return 1;
    if (saved_x != x || saved_y != y || saved_w != w || saved_h != h)
        return 1;
    return (uint32_t)(now_ms - last_capture_ms) >= min_capture_ms;
}

#endif
