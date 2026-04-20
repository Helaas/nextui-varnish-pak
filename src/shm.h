/*
 * shm.h — Daemon-side shared memory management for Varnish overlay slots.
 */

#ifndef VARNISH_SHM_WRITER_H
#define VARNISH_SHM_WRITER_H

#include <stdint.h>

int  shm_init(int fb_width, int fb_height);
void shm_slot_update(int slot_idx, int x, int y, int w, int h,
                     int z_order, const uint32_t *pixels);
void shm_slot_clear(int slot_idx);
void shm_cleanup(void);

#endif /* VARNISH_SHM_WRITER_H */
