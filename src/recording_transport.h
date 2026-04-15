/*
 * recording_transport.h — Shared-memory transport for Varnish video recording.
 *
 * The preload hook is the single producer. The daemon is the single consumer.
 * A producer lease prevents multiple hooked processes from interleaving frames
 * during menu -> MinArch handoff.
 */

#ifndef VARNISH_RECORDING_TRANSPORT_H
#define VARNISH_RECORDING_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define VARNISH_RECORDING_SHM_PATH          "/tmp/varnish_recording.dat"
#define VARNISH_RECORDING_MAGIC             0x56525243u  /* "VRRC" */
#define VARNISH_RECORDING_MAX_W             1280
#define VARNISH_RECORDING_MAX_H              768
#define VARNISH_RECORDING_RING_SLOTS          4
#define VARNISH_RECORDING_DEFAULT_CADENCE_MS 125u
#define VARNISH_RECORDING_DEFAULT_OUT_W      512
#define VARNISH_RECORDING_DEFAULT_OUT_H      384
#define VARNISH_RECORDING_PRODUCER_LEASE_MS 1000u

typedef struct {
    volatile uint32_t seq;
    volatile uint32_t frame_id;
    volatile int32_t  producer_pid;
    int               width;
    int               height;
    int               pitch;
    uint32_t          pixels[VARNISH_RECORDING_MAX_W * VARNISH_RECORDING_MAX_H];
} varnish_recording_frame_t;

typedef struct {
    uint32_t                    magic;
    volatile uint32_t           active;
    volatile uint32_t           cadence_ms;
    int                         source_w;
    int                         source_h;
    int                         output_w;
    int                         output_h;
    volatile int32_t            producer_pid;
    volatile uint32_t           producer_deadline_ms;
    volatile uint32_t           write_index;
    volatile uint32_t           read_index;
    volatile uint32_t           last_frame_id;
    volatile uint32_t           drop_count;
    varnish_recording_frame_t   frames[VARNISH_RECORDING_RING_SLOTS];
} varnish_recording_shm_t;

typedef struct {
    uint32_t frame_id;
    int32_t  producer_pid;
    int      width;
    int      height;
    int      pitch;
} varnish_recording_frame_info;

static inline int varnish_recording_deadline_reached(uint32_t now_ms,
                                                     uint32_t deadline_ms) {
    if (deadline_ms == 0u)
        return 1;
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static inline void varnish_recording_reset(varnish_recording_shm_t *shm,
                                           int source_w, int source_h) {
    if (!shm)
        return;

    memset(shm, 0, sizeof(*shm));
    shm->magic = VARNISH_RECORDING_MAGIC;
    shm->cadence_ms = VARNISH_RECORDING_DEFAULT_CADENCE_MS;
    shm->source_w = source_w;
    shm->source_h = source_h;
    shm->output_w = VARNISH_RECORDING_DEFAULT_OUT_W;
    shm->output_h = VARNISH_RECORDING_DEFAULT_OUT_H;
}

static inline void varnish_recording_set_active(varnish_recording_shm_t *shm,
                                                int active,
                                                uint32_t cadence_ms,
                                                int output_w,
                                                int output_h) {
    if (!shm)
        return;

    shm->cadence_ms = cadence_ms ? cadence_ms : VARNISH_RECORDING_DEFAULT_CADENCE_MS;
    shm->output_w = output_w > 0 ? output_w : VARNISH_RECORDING_DEFAULT_OUT_W;
    shm->output_h = output_h > 0 ? output_h : VARNISH_RECORDING_DEFAULT_OUT_H;

    if (active) {
        shm->active = 0u;
        __sync_synchronize();
        shm->read_index = 0u;
        shm->write_index = 0u;
        shm->last_frame_id = 0u;
        shm->drop_count = 0u;
        shm->producer_pid = 0;
        shm->producer_deadline_ms = 0u;
        __sync_synchronize();
        shm->active = 1u;
        __sync_synchronize();
        return;
    }

    shm->active = 0u;
    __sync_synchronize();
    shm->producer_pid = 0;
    shm->producer_deadline_ms = 0u;
    __sync_synchronize();
}

static inline int varnish_recording_try_acquire_producer(
    varnish_recording_shm_t *shm, int32_t producer_pid, uint32_t now_ms) {
    int attempt;

    if (!shm || !shm->active || producer_pid <= 0)
        return 0;

    for (attempt = 0; attempt < 4; attempt++) {
        int32_t owner = shm->producer_pid;
        uint32_t deadline = shm->producer_deadline_ms;

        if (owner == producer_pid) {
            shm->producer_deadline_ms = now_ms + VARNISH_RECORDING_PRODUCER_LEASE_MS;
            __sync_synchronize();
            return 1;
        }

        if (owner != 0 &&
            !varnish_recording_deadline_reached(now_ms, deadline)) {
            return 0;
        }

        if (__sync_bool_compare_and_swap(&shm->producer_pid, owner, producer_pid)) {
            __sync_synchronize();
            shm->producer_deadline_ms = now_ms + VARNISH_RECORDING_PRODUCER_LEASE_MS;
            __sync_synchronize();
            return 1;
        }
    }

    return 0;
}

static inline void varnish_recording_begin_slot_write(
    varnish_recording_frame_t *frame) {
    frame->seq++;
    __sync_synchronize();
}

static inline void varnish_recording_finish_slot_write(
    varnish_recording_frame_t *frame) {
    __sync_synchronize();
    frame->seq++;
    __sync_synchronize();
}

static inline uint32_t *varnish_recording_begin_frame(
    varnish_recording_shm_t *shm, int32_t producer_pid, int width, int height,
    uint32_t *out_slot_index, varnish_recording_frame_info *out_info) {
    uint32_t write_index;
    uint32_t read_index;
    uint32_t next_index;
    varnish_recording_frame_t *frame;
    uint32_t frame_id;

    if (!shm || !shm->active || width <= 0 || height <= 0 ||
        width > VARNISH_RECORDING_MAX_W || height > VARNISH_RECORDING_MAX_H) {
        return NULL;
    }

    write_index = shm->write_index;
    read_index = shm->read_index;
    next_index = (write_index + 1u) % VARNISH_RECORDING_RING_SLOTS;
    if (next_index == read_index) {
        __sync_add_and_fetch(&shm->drop_count, 1u);
        return NULL;
    }

    frame = &shm->frames[write_index];
    frame_id = __sync_add_and_fetch(&shm->last_frame_id, 1u);

    varnish_recording_begin_slot_write(frame);
    frame->frame_id = frame_id;
    frame->producer_pid = producer_pid;
    frame->width = width;
    frame->height = height;
    frame->pitch = width * (int)sizeof(uint32_t);

    if (out_slot_index)
        *out_slot_index = write_index;
    if (out_info) {
        out_info->frame_id = frame_id;
        out_info->producer_pid = producer_pid;
        out_info->width = width;
        out_info->height = height;
        out_info->pitch = frame->pitch;
    }

    return frame->pixels;
}

static inline void varnish_recording_publish_frame(
    varnish_recording_shm_t *shm, uint32_t slot_index) {
    if (!shm || slot_index >= VARNISH_RECORDING_RING_SLOTS)
        return;

    varnish_recording_finish_slot_write(&shm->frames[slot_index]);
    shm->write_index = (slot_index + 1u) % VARNISH_RECORDING_RING_SLOTS;
    __sync_synchronize();
}

static inline void varnish_recording_cancel_frame(
    varnish_recording_shm_t *shm, uint32_t slot_index) {
    if (!shm || slot_index >= VARNISH_RECORDING_RING_SLOTS)
        return;

    varnish_recording_finish_slot_write(&shm->frames[slot_index]);
}

static inline int varnish_recording_pop_frame(
    varnish_recording_shm_t *shm, uint32_t *dst_pixels, size_t dst_pixel_capacity,
    varnish_recording_frame_info *out_info) {
    uint32_t read_index;
    varnish_recording_frame_t *frame;
    int attempt;

    if (!shm || !dst_pixels)
        return 0;

    read_index = shm->read_index;
    if (read_index == shm->write_index)
        return 0;

    frame = &shm->frames[read_index];
    for (attempt = 0; attempt < 4; attempt++) {
        uint32_t seq1;
        uint32_t seq2;
        uint32_t frame_id;
        int32_t producer_pid;
        int width;
        int height;
        int pitch;
        size_t pixel_count;

        seq1 = frame->seq;
        __sync_synchronize();
        if (seq1 & 1u)
            continue;

        frame_id = frame->frame_id;
        producer_pid = frame->producer_pid;
        width = frame->width;
        height = frame->height;
        pitch = frame->pitch;
        if (width <= 0 || height <= 0 ||
            width > VARNISH_RECORDING_MAX_W ||
            height > VARNISH_RECORDING_MAX_H) {
            break;
        }

        pixel_count = (size_t)width * (size_t)height;
        if (pixel_count > dst_pixel_capacity)
            break;

        memcpy(dst_pixels, (const void *)frame->pixels,
               pixel_count * sizeof(uint32_t));

        __sync_synchronize();
        seq2 = frame->seq;
        if (seq1 == seq2 && !(seq2 & 1u)) {
            if (out_info) {
                out_info->frame_id = frame_id;
                out_info->producer_pid = producer_pid;
                out_info->width = width;
                out_info->height = height;
                out_info->pitch = pitch;
            }
            shm->read_index = (read_index + 1u) % VARNISH_RECORDING_RING_SLOTS;
            __sync_synchronize();
            return 1;
        }
    }

    shm->read_index = (read_index + 1u) % VARNISH_RECORDING_RING_SLOTS;
    __sync_synchronize();
    return -1;
}

#endif /* VARNISH_RECORDING_TRANSPORT_H */
