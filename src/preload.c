/*
 * varnish_overlay.so — LD_PRELOAD hook for multi-slot flicker-free overlay.
 *
 * Intercepts SDL_RenderPresent so we can composite Varnish overlay slots into
 * the SDL renderer BEFORE the real present call.  Each slot is thus part of the
 * GPU-composed frame and presented atomically — zero flicker.
 *
 * The Varnish daemon renders pills into shared-memory slots; this hook reads
 * committed overlay frames from shared memory, uploads changed pixel data to
 * SDL textures, and draws them via SDL_RenderCopy right before the real
 * SDL_RenderPresent.
 *
 * SDL functions are resolved at runtime via dlsym (no -lSDL2 linkage needed).
 *
 * Build:  gcc -std=gnu11 -O2 -fPIC -shared -o varnish_overlay.so preload.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#include "varnish_shm.h"

/* ── SDL constants (avoid pulling in SDL headers) ───────────────── */

#define VR_SDL_PIXELFORMAT_ARGB8888     0x16362004u
#define VR_SDL_TEXTUREACCESS_STREAMING  1
#define VR_SDL_BLENDMODE_BLEND          1

/* ── SDL function pointers (resolved lazily via dlsym) ──────────── */

typedef void  (*fn_SDL_RenderPresent)(void *);
typedef void  (*fn_SDL_Delay)(uint32_t);
typedef int   (*fn_SDL_PollEvent)(void *);
typedef int   (*fn_SDL_GetRendererOutputSize)(void *, int *, int *);
typedef void *(*fn_SDL_CreateTexture)(void *, uint32_t, int, int, int);
typedef int   (*fn_SDL_UpdateTexture)(void *, const void *, const void *, int);
typedef int   (*fn_SDL_SetTextureBlendMode)(void *, int);
typedef int   (*fn_SDL_RenderCopy)(void *, void *, const void *, const void *);
typedef int   (*fn_SDL_RenderReadPixels)(void *, const void *, uint32_t, void *, int);
typedef void  (*fn_SDL_DestroyTexture)(void *);

static fn_SDL_RenderPresent       real_present;
static fn_SDL_Delay               real_delay;
static fn_SDL_PollEvent           real_poll_event;
static fn_SDL_GetRendererOutputSize pfn_GetRendererOutputSize;
static fn_SDL_CreateTexture       pfn_CreateTexture;
static fn_SDL_UpdateTexture       pfn_UpdateTexture;
static fn_SDL_SetTextureBlendMode pfn_SetBlendMode;
static fn_SDL_RenderCopy          pfn_RenderCopy;
static fn_SDL_RenderReadPixels    pfn_RenderReadPixels;
static fn_SDL_DestroyTexture      pfn_DestroyTexture;
static int sdl_funcs_ok;

/* ── Shared memory state (lazy init) ────────────────────────────── */

static varnish_shm_t *shm;
static int shm_fd = -1;
static int shm_ok;

/* ── Per-slot cached overlay state ──────────────────────────────── */

static uint32_t slot_staging[VARNISH_MAX_SLOTS][VARNISH_SLOT_MAX_W * VARNISH_SLOT_MAX_H];
static uint32_t slot_cached_pixels[VARNISH_MAX_SLOTS][VARNISH_SLOT_MAX_W * VARNISH_SLOT_MAX_H];
static int      slot_cached_x[VARNISH_MAX_SLOTS];
static int      slot_cached_y[VARNISH_MAX_SLOTS];
static int      slot_cached_w[VARNISH_MAX_SLOTS];
static int      slot_cached_h[VARNISH_MAX_SLOTS];
static int      slot_cached_active[VARNISH_MAX_SLOTS];
static int      slot_cached_z[VARNISH_MAX_SLOTS];
static uint32_t slot_cached_frame_id[VARNISH_MAX_SLOTS];

/* ── Per-slot SDL textures ──────────────────────────────────────── */

static void    *slot_texture[VARNISH_MAX_SLOTS];
static void    *slot_tex_renderer[VARNISH_MAX_SLOTS];
static int      slot_tex_w[VARNISH_MAX_SLOTS];
static int      slot_tex_h[VARNISH_MAX_SLOTS];
static uint32_t slot_texture_frame_id[VARNISH_MAX_SLOTS];

/* ── Idle-present tracking ─────────────────────────────────────── */

static void *last_renderer;
static pid_t render_tid = -1;
static uint32_t last_present_frame_ids[VARNISH_MAX_SLOTS];
static __thread int force_present_guard;

/* ── Cached base-frame replay for idle overlay presents ────────── */

static uint32_t *base_frame_pixels;
static size_t    base_frame_capacity;
static void     *base_frame_texture;
static void     *base_frame_renderer;
static int       base_frame_w;
static int       base_frame_h;
static int       base_frame_valid;
static uint64_t  base_frame_captured_at_ms;

/* ── Init helpers ───────────────────────────────────────────────── */

static pid_t current_tid(void) {
    return (pid_t)syscall(SYS_gettid);
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void init_sdl_funcs(void) {
    real_delay       = (fn_SDL_Delay)dlsym(RTLD_NEXT, "SDL_Delay");
    real_poll_event  = (fn_SDL_PollEvent)dlsym(RTLD_NEXT, "SDL_PollEvent");
    pfn_GetRendererOutputSize = (fn_SDL_GetRendererOutputSize)dlsym(RTLD_NEXT, "SDL_GetRendererOutputSize");
    pfn_CreateTexture = (fn_SDL_CreateTexture)dlsym(RTLD_NEXT, "SDL_CreateTexture");
    pfn_UpdateTexture = (fn_SDL_UpdateTexture)dlsym(RTLD_NEXT, "SDL_UpdateTexture");
    pfn_SetBlendMode  = (fn_SDL_SetTextureBlendMode)dlsym(RTLD_NEXT, "SDL_SetTextureBlendMode");
    pfn_RenderCopy    = (fn_SDL_RenderCopy)dlsym(RTLD_NEXT, "SDL_RenderCopy");
    pfn_RenderReadPixels = (fn_SDL_RenderReadPixels)dlsym(RTLD_NEXT, "SDL_RenderReadPixels");
    pfn_DestroyTexture = (fn_SDL_DestroyTexture)dlsym(RTLD_NEXT, "SDL_DestroyTexture");

    sdl_funcs_ok = pfn_CreateTexture && pfn_UpdateTexture &&
                   pfn_SetBlendMode && pfn_RenderCopy && pfn_DestroyTexture;
}

static void init_shm(void) {
    shm_fd = open(VARNISH_SHM_PATH, O_RDONLY);
    if (shm_fd < 0) return;

    shm = (varnish_shm_t *)mmap(
        NULL, sizeof(varnish_shm_t),
        PROT_READ, MAP_SHARED, shm_fd, 0);

    if (shm == MAP_FAILED) {
        shm = NULL; close(shm_fd); shm_fd = -1; return;
    }

    if (shm->magic != VARNISH_SHM_MAGIC) {
        munmap(shm, sizeof(varnish_shm_t));
        shm = NULL; close(shm_fd); shm_fd = -1; return;
    }

    /* Initialize frame ID tracking */
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++) {
        slot_cached_frame_id[i] = UINT32_MAX;
        slot_texture_frame_id[i] = UINT32_MAX;
        last_present_frame_ids[i] = UINT32_MAX;
    }

    shm_ok = 1;
}

/* ── Per-slot seqlock read helpers ──────────────────────────────── */

static int slot_read_committed_frame_id(int idx, uint32_t *frame_id) {
    const volatile varnish_slot_t *slot = &shm->slots[idx];
    int attempt;

    for (attempt = 0; attempt < 4; attempt++) {
        uint32_t seq1, seq2, id;

        seq1 = slot->seq;
        __sync_synchronize();
        if (seq1 & 1u) continue;

        id = slot->frame_id;

        __sync_synchronize();
        seq2 = slot->seq;
        if (seq1 == seq2 && !(seq2 & 1u)) {
            if (frame_id) *frame_id = id;
            return 0;
        }
    }

    return -1;
}

static int slot_refresh_cache(int idx) {
    const volatile varnish_slot_t *slot = &shm->slots[idx];
    int attempt;

    for (attempt = 0; attempt < 4; attempt++) {
        uint32_t seq1, seq2, new_frame_id;
        int new_active, new_x, new_y, new_w, new_h, new_z;

        seq1 = slot->seq;
        __sync_synchronize();
        if (seq1 & 1u) continue;

        new_frame_id = slot->frame_id;
        new_active   = slot->active;
        new_x        = slot->x;
        new_y        = slot->y;
        new_w        = slot->w;
        new_h        = slot->h;
        new_z        = slot->z_order;

        if (new_frame_id != slot_cached_frame_id[idx] && new_active) {
            if (new_w <= 0 || new_h <= 0 ||
                new_w > VARNISH_SLOT_MAX_W || new_h > VARNISH_SLOT_MAX_H) {
                continue;
            }
            memcpy(slot_staging[idx], (const void *)slot->pixels,
                   (size_t)new_w * (size_t)new_h * sizeof(uint32_t));
        }

        __sync_synchronize();
        seq2 = slot->seq;
        if (seq1 != seq2 || (seq2 & 1u)) continue;

        if (new_frame_id == slot_cached_frame_id[idx])
            return 0;

        if (!new_active) {
            slot_cached_active[idx] = 0;
            slot_cached_x[idx] = 0;
            slot_cached_y[idx] = 0;
            slot_cached_w[idx] = 0;
            slot_cached_h[idx] = 0;
            slot_cached_z[idx] = 0;
            slot_cached_frame_id[idx] = new_frame_id;
            return 1;
        }

        memcpy(slot_cached_pixels[idx], slot_staging[idx],
               (size_t)new_w * (size_t)new_h * sizeof(uint32_t));
        slot_cached_active[idx] = 1;
        slot_cached_x[idx] = new_x;
        slot_cached_y[idx] = new_y;
        slot_cached_w[idx] = new_w;
        slot_cached_h[idx] = new_h;
        slot_cached_z[idx] = new_z;
        slot_cached_frame_id[idx] = new_frame_id;
        return 1;
    }

    return 0;
}

/* ── Multi-slot overlay drawing ─────────────────────────────────── */

static void draw_slot(void *renderer, int idx) {
    typedef struct { int x, y, w, h; } SDL_Rect;
    SDL_Rect dst;

    slot_refresh_cache(idx);

    if (!slot_cached_active[idx] || slot_cached_w[idx] <= 0 || slot_cached_h[idx] <= 0)
        return;

    /* Recreate texture if renderer or dimensions changed */
    if (slot_texture[idx] &&
        (slot_tex_renderer[idx] != renderer ||
         slot_tex_w[idx] != slot_cached_w[idx] ||
         slot_tex_h[idx] != slot_cached_h[idx])) {
        pfn_DestroyTexture(slot_texture[idx]);
        slot_texture[idx] = NULL;
        slot_texture_frame_id[idx] = UINT32_MAX;
    }

    if (!slot_texture[idx]) {
        slot_texture[idx] = pfn_CreateTexture(
            renderer, VR_SDL_PIXELFORMAT_ARGB8888,
            VR_SDL_TEXTUREACCESS_STREAMING,
            slot_cached_w[idx], slot_cached_h[idx]);
        if (!slot_texture[idx]) return;
        pfn_SetBlendMode(slot_texture[idx], VR_SDL_BLENDMODE_BLEND);
        slot_tex_renderer[idx] = renderer;
        slot_tex_w[idx] = slot_cached_w[idx];
        slot_tex_h[idx] = slot_cached_h[idx];
        slot_texture_frame_id[idx] = UINT32_MAX;
    }

    if (slot_texture_frame_id[idx] != slot_cached_frame_id[idx]) {
        pfn_UpdateTexture(slot_texture[idx], NULL, slot_cached_pixels[idx],
                          slot_cached_w[idx] * (int)sizeof(uint32_t));
        slot_texture_frame_id[idx] = slot_cached_frame_id[idx];
    }

    dst.x = slot_cached_x[idx];
    dst.y = slot_cached_y[idx];
    dst.w = slot_cached_w[idx];
    dst.h = slot_cached_h[idx];
    pfn_RenderCopy(renderer, slot_texture[idx], NULL, &dst);
}

static int collect_active_slots(int *order) {
    int count = 0;
    int i, j;

    if (!shm || !shm_ok) {
        if (shm_fd >= 0) return 0;
        init_shm();
        if (!shm_ok) return 0;
    }

    for (i = 0; i < VARNISH_MAX_SLOTS; i++) {
        slot_refresh_cache(i);
        if (slot_cached_active[i])
            order[count++] = i;
    }

    for (i = 1; i < count; i++) {
        int key = order[i];
        int key_z = slot_cached_z[key];
        j = i - 1;
        while (j >= 0 && slot_cached_z[order[j]] > key_z) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    return count;
}

static int draw_all_overlays(void *renderer) {
    int order[VARNISH_MAX_SLOTS];
    int count = collect_active_slots(order);
    int i;

    /* Draw slots in z_order (lowest first) */
    for (i = 0; i < count; i++)
        draw_slot(renderer, order[i]);

    return count;
}

/* ── Base-frame capture / replay ───────────────────────────────── */

static void discard_base_frame_texture(void) {
    if (base_frame_texture && pfn_DestroyTexture)
        pfn_DestroyTexture(base_frame_texture);
    base_frame_texture = NULL;
    base_frame_renderer = NULL;
    base_frame_w = 0;
    base_frame_h = 0;
    base_frame_valid = 0;
    base_frame_captured_at_ms = 0;
}

static int ensure_base_frame_pixels(size_t pixel_count) {
    uint32_t *pixels;

    if (pixel_count == 0) return 0;
    if (pixel_count <= base_frame_capacity) return 1;

    pixels = (uint32_t *)realloc(base_frame_pixels, pixel_count * sizeof(uint32_t));
    if (!pixels) return 0;

    base_frame_pixels = pixels;
    base_frame_capacity = pixel_count;
    return 1;
}

static int ensure_base_frame_texture(void *renderer, int width, int height) {
    if (!pfn_CreateTexture || !pfn_UpdateTexture || !renderer ||
        width <= 0 || height <= 0)
        return 0;

    if (base_frame_texture &&
        (base_frame_renderer != renderer ||
         base_frame_w != width ||
         base_frame_h != height)) {
        discard_base_frame_texture();
    }

    if (!base_frame_texture) {
        base_frame_texture = pfn_CreateTexture(
            renderer, VR_SDL_PIXELFORMAT_ARGB8888,
            VR_SDL_TEXTUREACCESS_STREAMING,
            width, height);
        if (!base_frame_texture)
            return 0;
        base_frame_renderer = renderer;
        base_frame_w = width;
        base_frame_h = height;
    }

    return 1;
}

static void capture_base_frame(void *renderer) {
    int width, height;
    size_t pixel_count;

    if (!renderer || !pfn_GetRendererOutputSize || !pfn_RenderReadPixels)
        return;

    if (pfn_GetRendererOutputSize(renderer, &width, &height) != 0 ||
        width <= 0 || height <= 0)
        return;

    pixel_count = (size_t)width * (size_t)height;
    if (!ensure_base_frame_pixels(pixel_count))
        return;
    if (!ensure_base_frame_texture(renderer, width, height))
        return;

    if (pfn_RenderReadPixels(renderer, NULL, VR_SDL_PIXELFORMAT_ARGB8888,
                             base_frame_pixels,
                             width * (int)sizeof(uint32_t)) != 0)
        return;

    if (pfn_UpdateTexture(base_frame_texture, NULL, base_frame_pixels,
                          width * (int)sizeof(uint32_t)) != 0)
        return;

    base_frame_valid = 1;
    base_frame_captured_at_ms = monotonic_ms();
}

static int replay_base_frame(void *renderer) {
    if (!base_frame_valid || !renderer || !pfn_RenderCopy)
        return 0;
    if (!base_frame_texture || base_frame_renderer != renderer)
        return 0;

    pfn_RenderCopy(renderer, base_frame_texture, NULL, NULL);
    return 1;
}

/* ── Idle-present forcing ──────────────────────────────────────── */

static void maybe_force_idle_present(void) {
    int i;
    int need_present = 0;

    if (force_present_guard) return;
    if (!last_renderer || render_tid < 0) return;
    if (!real_present) return;
    if (current_tid() != render_tid) return;

    if (!shm || !shm_ok) {
        if (shm_fd >= 0) return;
        init_shm();
        if (!shm_ok) return;
    }

    /* Check if any slot has a new frame we haven't presented yet */
    for (i = 0; i < VARNISH_MAX_SLOTS; i++) {
        uint32_t frame_id;
        if (slot_read_committed_frame_id(i, &frame_id) == 0 &&
            frame_id != last_present_frame_ids[i]) {
            need_present = 1;
            break;
        }
    }

    if (!need_present) return;

    force_present_guard = 1;
    if (!replay_base_frame(last_renderer)) {
        force_present_guard = 0;
        return;
    }
    if (__builtin_expect(sdl_funcs_ok, 1))
        draw_all_overlays(last_renderer);
    real_present(last_renderer);
    for (i = 0; i < VARNISH_MAX_SLOTS; i++)
        last_present_frame_ids[i] = slot_cached_frame_id[i];
    force_present_guard = 0;
}

/* ── SDL_RenderPresent interposition ────────────────────────────── */

void SDL_RenderPresent(void *renderer) {
    int overlay_count = 0;
    uint64_t now_ms;
    int should_capture = 0;

    if (__builtin_expect(!real_present, 0)) {
        real_present = (fn_SDL_RenderPresent)dlsym(RTLD_NEXT, "SDL_RenderPresent");
        if (!real_present) return;
        init_sdl_funcs();
    }

    last_renderer = renderer;
    render_tid = current_tid();

    overlay_count = collect_active_slots((int [VARNISH_MAX_SLOTS]){0});
    now_ms = monotonic_ms();
    should_capture = !base_frame_valid || overlay_count > 0;
    if (!should_capture && now_ms > 0 &&
        (!base_frame_captured_at_ms ||
         now_ms - base_frame_captured_at_ms >= 250u)) {
        should_capture = 1;
    }
    if (should_capture)
        capture_base_frame(renderer);

    /* Draw overlays INTO the renderer before presenting */
    if (__builtin_expect(sdl_funcs_ok, 1))
        draw_all_overlays(renderer);

    real_present(renderer);
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++)
        last_present_frame_ids[i] = slot_cached_frame_id[i];
}

void SDL_Delay(uint32_t ms) {
    if (__builtin_expect(!real_delay, 0)) {
        real_delay = (fn_SDL_Delay)dlsym(RTLD_NEXT, "SDL_Delay");
        if (!real_delay) {
            usleep((useconds_t)ms * 1000u);
            return;
        }
    }

    maybe_force_idle_present();
    real_delay(ms);
}

int SDL_PollEvent(void *event) {
    if (__builtin_expect(!real_poll_event, 0)) {
        real_poll_event = (fn_SDL_PollEvent)dlsym(RTLD_NEXT, "SDL_PollEvent");
        if (!real_poll_event) return 0;
    }

    maybe_force_idle_present();
    return real_poll_event(event);
}
