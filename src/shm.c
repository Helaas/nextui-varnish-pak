/*
 * shm.c — Daemon-side shared memory management for Varnish overlay slots.
 *
 * Creates and owns the SHM file at VARNISH_SHM_PATH.  Provides seqlock-
 * protected writes so the preload hook (reader) can safely read slot data
 * without locks.
 */

#include "shm.h"
#include "varnish_shm.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static varnish_shm_t *shm_ptr;
static int shm_fd = -1;
static uint32_t slot_frame_ids[VARNISH_MAX_SLOTS];

/* ── Seqlock helpers ───────────────────────────────────────────── */

static void slot_begin_write(int idx) {
    shm_ptr->slots[idx].seq++;
    __sync_synchronize();
}

static void slot_finish_write(int idx) {
    __sync_synchronize();
    shm_ptr->slots[idx].seq++;
    __sync_synchronize();
}

/* ── Public API ────────────────────────────────────────────────── */

int shm_init(int fb_width, int fb_height) {
    shm_fd = open(VARNISH_SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (shm_fd < 0) {
        perror("varnish: shm open");
        return -1;
    }

    if (ftruncate(shm_fd, (off_t)sizeof(varnish_shm_t)) < 0) {
        perror("varnish: shm ftruncate");
        close(shm_fd);
        shm_fd = -1;
        return -1;
    }

    shm_ptr = (varnish_shm_t *)mmap(
        NULL, sizeof(varnish_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("varnish: shm mmap");
        shm_ptr = NULL;
        close(shm_fd);
        shm_fd = -1;
        return -1;
    }

    memset(shm_ptr, 0, sizeof(*shm_ptr));
    shm_ptr->fb_width = fb_width;
    shm_ptr->fb_height = fb_height;
    shm_ptr->magic = VARNISH_SHM_MAGIC;
    __sync_synchronize();

    memset(slot_frame_ids, 0, sizeof(slot_frame_ids));

    fprintf(stderr, "varnish: shm ready at %s (%dx%d)\n",
            VARNISH_SHM_PATH, fb_width, fb_height);
    return 0;
}

void shm_slot_update(int slot_idx, int x, int y, int w, int h,
                     int z_order, const uint32_t *pixels) {
    varnish_slot_t *slot;

    if (!shm_ptr || slot_idx < 0 || slot_idx >= VARNISH_MAX_SLOTS) return;
    if (!pixels || w <= 0 || h <= 0) return;
    if (w > VARNISH_SLOT_MAX_W || h > VARNISH_SLOT_MAX_H) return;

    slot = &shm_ptr->slots[slot_idx];

    slot_begin_write(slot_idx);

    slot->x       = x;
    slot->y       = y;
    slot->w       = w;
    slot->h       = h;
    slot->z_order = z_order;

    memcpy((void *)slot->pixels, pixels,
           (size_t)w * (size_t)h * sizeof(uint32_t));

    slot->active = 1;
    slot_frame_ids[slot_idx]++;
    slot->frame_id = slot_frame_ids[slot_idx];

    slot_finish_write(slot_idx);
}

void shm_slot_clear(int slot_idx) {
    varnish_slot_t *slot;
    int already_clear;

    if (!shm_ptr || slot_idx < 0 || slot_idx >= VARNISH_MAX_SLOTS) return;

    slot = &shm_ptr->slots[slot_idx];

    already_clear = (slot->active == 0 &&
                     slot->x == 0 && slot->y == 0 &&
                     slot->w == 0 && slot->h == 0);
    if (already_clear) return;

    slot_begin_write(slot_idx);
    slot->active  = 0;
    slot->x       = 0;
    slot->y       = 0;
    slot->w       = 0;
    slot->h       = 0;
    slot->z_order = 0;
    slot_frame_ids[slot_idx]++;
    slot->frame_id = slot_frame_ids[slot_idx];
    slot_finish_write(slot_idx);
}

void shm_cleanup(void) {
    if (shm_ptr) {
        for (int i = 0; i < VARNISH_MAX_SLOTS; i++)
            shm_slot_clear(i);
        munmap(shm_ptr, sizeof(varnish_shm_t));
        shm_ptr = NULL;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
    unlink(VARNISH_SHM_PATH);
}
