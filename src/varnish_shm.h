/*
 * varnish_shm.h — Multi-slot shared memory layout for Varnish overlay system.
 *
 * Shared between the preload hook (reader, runs inside nextui.elf) and the
 * Varnish daemon (writer).  Each slot has its own seqlock so the daemon can
 * update one slot without blocking reads of other slots.
 *
 * Pixel format: ARGB8888 stored as uint32_t — ((a<<24)|(r<<16)|(g<<8)|b).
 * The preload hook creates textures with SDL_PIXELFORMAT_ARGB8888 to match.
 */

#ifndef VARNISH_SHM_H
#define VARNISH_SHM_H

#include <stdint.h>

#define VARNISH_MAX_SLOTS    8
#define VARNISH_SHM_PATH     "/tmp/varnish_overlay.dat"
#define VARNISH_SHM_MAGIC    0x56524E53u  /* "VRNS" */
#define VARNISH_SLOT_MAX_W   512
#define VARNISH_SLOT_MAX_H   128

typedef struct {
    volatile uint32_t seq;       /* Per-slot seqlock: odd while writing, even when committed */
    volatile uint32_t frame_id;  /* Incremented once per committed overlay state change */
    int      active;             /* 1 = draw overlay, 0 = skip */
    int      x, y, w, h;        /* Screen-space position and size in pixels */
    int      z_order;            /* Lower values drawn first (behind higher values) */
    uint32_t pixels[VARNISH_SLOT_MAX_W * VARNISH_SLOT_MAX_H]; /* ARGB8888 */
} varnish_slot_t;

typedef struct {
    uint32_t magic;              /* VARNISH_SHM_MAGIC when valid */
    int      fb_width;           /* Expected screen width */
    int      fb_height;          /* Expected screen height */
    int      slot_count;         /* Active slot count hint (informational) */
    varnish_slot_t slots[VARNISH_MAX_SLOTS];
} varnish_shm_t;

#endif /* VARNISH_SHM_H */
