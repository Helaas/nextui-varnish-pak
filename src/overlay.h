/*
 * overlay.h — Pill rendering for Varnish overlay system.
 */

#ifndef VARNISH_OVERLAY_H
#define VARNISH_OVERLAY_H

#include <stdint.h>
#include "varnish_shm.h"

typedef enum {
    VARNISH_POS_TOP_LEFT,
    VARNISH_POS_TOP_CENTER,
    VARNISH_POS_TOP_RIGHT,
    VARNISH_POS_BOTTOM_LEFT,
    VARNISH_POS_BOTTOM_CENTER,
    VARNISH_POS_BOTTOM_RIGHT,
} varnish_position_t;

/* Parse a position string (e.g. "bottom-center") to enum. Returns -1 on failure. */
int overlay_parse_position(const char *str);

/* Initialize overlay rendering resources (SDL, TTF, font, theme).
   Call once at daemon startup. fb_width/fb_height are screen dimensions. */
int overlay_init(int fb_width, int fb_height);

/* Render a pill with the given text and position.
   Writes ARGB8888 pixels into out_pixels (must be VARNISH_SLOT_MAX_W * VARNISH_SLOT_MAX_H).
   Returns 0 on success, fills out_x/y/w/h with screen position. */
int overlay_render_pill(const char *text, varnish_position_t position,
                        int *out_x, int *out_y, int *out_w, int *out_h,
                        uint32_t *out_pixels);

/* Release overlay resources. */
void overlay_cleanup(void);

#endif /* VARNISH_OVERLAY_H */
