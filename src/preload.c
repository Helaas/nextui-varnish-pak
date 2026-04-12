/*
 * varnish_overlay.so — LD_PRELOAD hook for multi-slot flicker-free overlay.
 *
 * Intercepts SDL_RenderPresent and SDL_GL_SwapWindow so we can composite
 * Varnish overlay slots into SDL 2D and MinArch's OpenGL path before the real
 * present/swap call. Each slot is thus part of the composed frame and
 * presented atomically — zero flicker.
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
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/mman.h>

#include "varnish_shm.h"

/* ── SDL constants (avoid pulling in SDL headers) ───────────────── */

#define VR_SDL_PIXELFORMAT_ARGB8888     0x16362004u
#define VR_SDL_TEXTUREACCESS_STREAMING  1
#define VR_SDL_BLENDMODE_BLEND          1

/* ── Minimal GL declarations (resolved at runtime) ─────────────── */

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef ptrdiff_t GLsizeiptr;
typedef char GLchar;

#define VR_GL_FALSE                        0
#define VR_GL_TRUE                         1
#define VR_GL_TEXTURE_2D                   0x0DE1u
#define VR_GL_TEXTURE0                     0x84C0u
#define VR_GL_TEXTURE_BINDING_2D           0x8069u
#define VR_GL_TEXTURE_MIN_FILTER           0x2801u
#define VR_GL_TEXTURE_MAG_FILTER           0x2800u
#define VR_GL_TEXTURE_WRAP_S               0x2802u
#define VR_GL_TEXTURE_WRAP_T               0x2803u
#define VR_GL_CLAMP_TO_EDGE                0x812Fu
#define VR_GL_LINEAR                       0x2601u
#define VR_GL_RGBA                         0x1908u
#define VR_GL_UNSIGNED_BYTE                0x1401u
#define VR_GL_FLOAT                        0x1406u
#define VR_GL_BLEND                        0x0BE2u
#define VR_GL_CULL_FACE                    0x0B44u
#define VR_GL_DEPTH_TEST                   0x0B71u
#define VR_GL_SCISSOR_TEST                 0x0C11u
#define VR_GL_SRC_ALPHA                    0x0302u
#define VR_GL_ONE_MINUS_SRC_ALPHA          0x0303u
#define VR_GL_VIEWPORT                     0x0BA2u
#define VR_GL_ARRAY_BUFFER                 0x8892u
#define VR_GL_ARRAY_BUFFER_BINDING         0x8894u
#define VR_GL_STATIC_DRAW                  0x88E4u
#define VR_GL_TRIANGLE_STRIP               0x0005u
#define VR_GL_VERTEX_SHADER                0x8B31u
#define VR_GL_FRAGMENT_SHADER              0x8B30u
#define VR_GL_COMPILE_STATUS               0x8B81u
#define VR_GL_LINK_STATUS                  0x8B82u
#define VR_GL_CURRENT_PROGRAM              0x8B8Du
#define VR_GL_ACTIVE_TEXTURE               0x84E0u
#define VR_GL_BLEND_DST_RGB               0x80C8u
#define VR_GL_BLEND_SRC_RGB               0x80C9u
#define VR_GL_VERTEX_ARRAY_BINDING         0x85B5u
#define VR_GL_VERTEX_ATTRIB_ARRAY_ENABLED  0x8622u

/* ── SDL function pointers (resolved lazily via dlsym) ──────────── */

typedef void  (*fn_SDL_RenderPresent)(void *);
typedef void  (*fn_SDL_GL_SwapWindow)(void *);
typedef void  (*fn_SDL_Delay)(uint32_t);
typedef int   (*fn_SDL_PollEvent)(void *);
typedef void *(*fn_SDL_CreateTexture)(void *, uint32_t, int, int, int);
typedef int   (*fn_SDL_UpdateTexture)(void *, const void *, const void *, int);
typedef int   (*fn_SDL_SetTextureBlendMode)(void *, int);
typedef int   (*fn_SDL_RenderCopy)(void *, void *, const void *, const void *);
typedef int   (*fn_SDL_RenderReadPixels)(void *, const void *, uint32_t, void *, int);
typedef void  (*fn_SDL_DestroyTexture)(void *);
typedef void *(*fn_SDL_GL_GetProcAddress)(const char *);
typedef void *(*fn_SDL_GL_GetCurrentContext)(void);
typedef void  (*fn_SDL_GL_GetDrawableSize)(void *, int *, int *);

typedef GLuint    (*fn_glCreateShader)(GLenum);
typedef void      (*fn_glShaderSource)(GLuint, GLsizei, const GLchar *const*, const GLint *);
typedef void      (*fn_glCompileShader)(GLuint);
typedef void      (*fn_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void      (*fn_glDeleteShader)(GLuint);
typedef GLuint    (*fn_glCreateProgram)(void);
typedef void      (*fn_glAttachShader)(GLuint, GLuint);
typedef void      (*fn_glLinkProgram)(GLuint);
typedef void      (*fn_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void      (*fn_glDeleteProgram)(GLuint);
typedef void      (*fn_glUseProgram)(GLuint);
typedef GLint     (*fn_glGetAttribLocation)(GLuint, const GLchar *);
typedef GLint     (*fn_glGetUniformLocation)(GLuint, const GLchar *);
typedef void      (*fn_glUniform1i)(GLint, GLint);
typedef void      (*fn_glGenTextures)(GLsizei, GLuint *);
typedef void      (*fn_glDeleteTextures)(GLsizei, const GLuint *);
typedef void      (*fn_glBindTexture)(GLenum, GLuint);
typedef void      (*fn_glTexParameteri)(GLenum, GLenum, GLint);
typedef void      (*fn_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void      (*fn_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void      (*fn_glActiveTexture)(GLenum);
typedef void      (*fn_glEnable)(GLenum);
typedef void      (*fn_glDisable)(GLenum);
typedef void      (*fn_glBlendFunc)(GLenum, GLenum);
typedef void      (*fn_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void      (*fn_glBindBuffer)(GLenum, GLuint);
typedef void      (*fn_glGenBuffers)(GLsizei, GLuint *);
typedef void      (*fn_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void      (*fn_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void      (*fn_glBindVertexArray)(GLuint);
typedef void      (*fn_glGenVertexArrays)(GLsizei, GLuint *);
typedef void      (*fn_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef void      (*fn_glEnableVertexAttribArray)(GLuint);
typedef void      (*fn_glDisableVertexAttribArray)(GLuint);
typedef void      (*fn_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void      (*fn_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void      (*fn_glGetIntegerv)(GLenum, GLint *);
typedef GLboolean (*fn_glIsEnabled)(GLenum);

static fn_SDL_RenderPresent       real_present;
static fn_SDL_GL_SwapWindow       real_gl_swap;
static fn_SDL_Delay               real_delay;
static fn_SDL_PollEvent           real_poll_event;
static fn_SDL_CreateTexture       pfn_CreateTexture;
static fn_SDL_UpdateTexture       pfn_UpdateTexture;
static fn_SDL_SetTextureBlendMode pfn_SetBlendMode;
static fn_SDL_RenderCopy          pfn_RenderCopy;
static fn_SDL_RenderReadPixels    pfn_RenderReadPixels;
static fn_SDL_DestroyTexture      pfn_DestroyTexture;
static fn_SDL_GL_GetProcAddress   pfn_SDL_GL_GetProcAddress;
static fn_SDL_GL_GetCurrentContext pfn_SDL_GL_GetCurrentContext;
static fn_SDL_GL_GetDrawableSize  pfn_SDL_GL_GetDrawableSize;
static int sdl_funcs_ok;

static fn_glCreateShader              pfn_glCreateShader;
static fn_glShaderSource              pfn_glShaderSource;
static fn_glCompileShader             pfn_glCompileShader;
static fn_glGetShaderiv               pfn_glGetShaderiv;
static fn_glDeleteShader              pfn_glDeleteShader;
static fn_glCreateProgram             pfn_glCreateProgram;
static fn_glAttachShader              pfn_glAttachShader;
static fn_glLinkProgram               pfn_glLinkProgram;
static fn_glGetProgramiv              pfn_glGetProgramiv;
static fn_glDeleteProgram             pfn_glDeleteProgram;
static fn_glUseProgram                pfn_glUseProgram;
static fn_glGetAttribLocation         pfn_glGetAttribLocation;
static fn_glGetUniformLocation        pfn_glGetUniformLocation;
static fn_glUniform1i                 pfn_glUniform1i;
static fn_glGenTextures               pfn_glGenTextures;
static fn_glDeleteTextures            pfn_glDeleteTextures;
static fn_glBindTexture               pfn_glBindTexture;
static fn_glTexParameteri             pfn_glTexParameteri;
static fn_glTexImage2D                pfn_glTexImage2D;
static fn_glTexSubImage2D             pfn_glTexSubImage2D;
static fn_glActiveTexture             pfn_glActiveTexture;
static fn_glEnable                    pfn_glEnable;
static fn_glDisable                   pfn_glDisable;
static fn_glBlendFunc                 pfn_glBlendFunc;
static fn_glViewport                  pfn_glViewport;
static fn_glBindBuffer                pfn_glBindBuffer;
static fn_glGenBuffers                pfn_glGenBuffers;
static fn_glDeleteBuffers             pfn_glDeleteBuffers;
static fn_glBufferData                pfn_glBufferData;
static fn_glBindVertexArray           pfn_glBindVertexArray;
static fn_glGenVertexArrays           pfn_glGenVertexArrays;
static fn_glDeleteVertexArrays        pfn_glDeleteVertexArrays;
static fn_glEnableVertexAttribArray   pfn_glEnableVertexAttribArray;
static fn_glDisableVertexAttribArray  pfn_glDisableVertexAttribArray;
static fn_glVertexAttribPointer       pfn_glVertexAttribPointer;
static fn_glDrawArrays                pfn_glDrawArrays;
static fn_glGetIntegerv               pfn_glGetIntegerv;
static fn_glIsEnabled                 pfn_glIsEnabled;
static int gl_funcs_ok;

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

static GLuint   gl_slot_texture[VARNISH_MAX_SLOTS];
static int      gl_slot_tex_w[VARNISH_MAX_SLOTS];
static int      gl_slot_tex_h[VARNISH_MAX_SLOTS];
static uint32_t gl_slot_texture_frame_id[VARNISH_MAX_SLOTS];
static uint8_t  gl_slot_rgba[VARNISH_MAX_SLOTS][VARNISH_SLOT_MAX_W * VARNISH_SLOT_MAX_H * 4];

static void    *gl_context;
static GLuint   gl_program;
static GLuint   gl_vbo;
static GLuint   gl_vao;
static GLint    gl_attr_position = -1;
static GLint    gl_attr_texcoord = -1;
static GLint    gl_uniform_texture = -1;
static int      gl_program_ready;

/* ── Idle-present tracking ─────────────────────────────────────── */

static void *last_renderer;
static pid_t render_tid = -1;
static uint32_t last_present_frame_ids[VARNISH_MAX_SLOTS];
static __thread int force_present_guard;

/* ── Per-slot saved background (region behind each pill) ───────── */

static uint32_t slot_save_pixels[VARNISH_MAX_SLOTS][VARNISH_SLOT_MAX_W * VARNISH_SLOT_MAX_H];
static void    *slot_save_texture[VARNISH_MAX_SLOTS];
static void    *slot_save_renderer[VARNISH_MAX_SLOTS];
static int      slot_save_x[VARNISH_MAX_SLOTS];
static int      slot_save_y[VARNISH_MAX_SLOTS];
static int      slot_save_w[VARNISH_MAX_SLOTS];
static int      slot_save_h[VARNISH_MAX_SLOTS];
static int      slot_save_valid[VARNISH_MAX_SLOTS];

/* ── Full frame save (clean host content for idle-present restore) ── */

#define FULL_FRAME_MAX_W   1280
#define FULL_FRAME_MAX_H    768

static uint32_t full_frame_pixels[FULL_FRAME_MAX_W * FULL_FRAME_MAX_H];
static void    *full_frame_texture;
static void    *full_frame_renderer;
static int      full_frame_w;
static int      full_frame_h;
static int      full_frame_valid;

/* ── Init helpers ───────────────────────────────────────────────── */

static pid_t current_tid(void) {
    return (pid_t)syscall(SYS_gettid);
}

static void *resolve_symbol_any(const char *name) {
    void *sym;

    if (!name || !name[0])
        return NULL;

    sym = dlsym(RTLD_NEXT, name);
    if (!sym)
        sym = dlsym(RTLD_DEFAULT, name);
    return sym;
}

static void init_sdl_funcs(void) {
    real_delay        = (fn_SDL_Delay)resolve_symbol_any("SDL_Delay");
    real_poll_event   = (fn_SDL_PollEvent)resolve_symbol_any("SDL_PollEvent");
    pfn_CreateTexture = (fn_SDL_CreateTexture)resolve_symbol_any("SDL_CreateTexture");
    pfn_UpdateTexture = (fn_SDL_UpdateTexture)resolve_symbol_any("SDL_UpdateTexture");
    pfn_SetBlendMode  = (fn_SDL_SetTextureBlendMode)resolve_symbol_any("SDL_SetTextureBlendMode");
    pfn_RenderCopy    = (fn_SDL_RenderCopy)resolve_symbol_any("SDL_RenderCopy");
    pfn_RenderReadPixels = (fn_SDL_RenderReadPixels)resolve_symbol_any("SDL_RenderReadPixels");
    pfn_DestroyTexture = (fn_SDL_DestroyTexture)resolve_symbol_any("SDL_DestroyTexture");
    pfn_SDL_GL_GetProcAddress =
        (fn_SDL_GL_GetProcAddress)resolve_symbol_any("SDL_GL_GetProcAddress");
    pfn_SDL_GL_GetCurrentContext =
        (fn_SDL_GL_GetCurrentContext)resolve_symbol_any("SDL_GL_GetCurrentContext");
    pfn_SDL_GL_GetDrawableSize =
        (fn_SDL_GL_GetDrawableSize)resolve_symbol_any("SDL_GL_GetDrawableSize");

    sdl_funcs_ok = pfn_CreateTexture && pfn_UpdateTexture &&
                   pfn_SetBlendMode && pfn_RenderCopy && pfn_DestroyTexture;
}

static void *resolve_gl_symbol(const char *name) {
    void *sym = NULL;

    if (pfn_SDL_GL_GetProcAddress)
        sym = pfn_SDL_GL_GetProcAddress(name);
    if (!sym)
        sym = resolve_symbol_any(name);
    return sym;
}

static void init_gl_funcs(void) {
    pfn_glCreateShader = (fn_glCreateShader)resolve_gl_symbol("glCreateShader");
    pfn_glShaderSource = (fn_glShaderSource)resolve_gl_symbol("glShaderSource");
    pfn_glCompileShader = (fn_glCompileShader)resolve_gl_symbol("glCompileShader");
    pfn_glGetShaderiv = (fn_glGetShaderiv)resolve_gl_symbol("glGetShaderiv");
    pfn_glDeleteShader = (fn_glDeleteShader)resolve_gl_symbol("glDeleteShader");
    pfn_glCreateProgram = (fn_glCreateProgram)resolve_gl_symbol("glCreateProgram");
    pfn_glAttachShader = (fn_glAttachShader)resolve_gl_symbol("glAttachShader");
    pfn_glLinkProgram = (fn_glLinkProgram)resolve_gl_symbol("glLinkProgram");
    pfn_glGetProgramiv = (fn_glGetProgramiv)resolve_gl_symbol("glGetProgramiv");
    pfn_glDeleteProgram = (fn_glDeleteProgram)resolve_gl_symbol("glDeleteProgram");
    pfn_glUseProgram = (fn_glUseProgram)resolve_gl_symbol("glUseProgram");
    pfn_glGetAttribLocation = (fn_glGetAttribLocation)resolve_gl_symbol("glGetAttribLocation");
    pfn_glGetUniformLocation = (fn_glGetUniformLocation)resolve_gl_symbol("glGetUniformLocation");
    pfn_glUniform1i = (fn_glUniform1i)resolve_gl_symbol("glUniform1i");
    pfn_glGenTextures = (fn_glGenTextures)resolve_gl_symbol("glGenTextures");
    pfn_glDeleteTextures = (fn_glDeleteTextures)resolve_gl_symbol("glDeleteTextures");
    pfn_glBindTexture = (fn_glBindTexture)resolve_gl_symbol("glBindTexture");
    pfn_glTexParameteri = (fn_glTexParameteri)resolve_gl_symbol("glTexParameteri");
    pfn_glTexImage2D = (fn_glTexImage2D)resolve_gl_symbol("glTexImage2D");
    pfn_glTexSubImage2D = (fn_glTexSubImage2D)resolve_gl_symbol("glTexSubImage2D");
    pfn_glActiveTexture = (fn_glActiveTexture)resolve_gl_symbol("glActiveTexture");
    pfn_glEnable = (fn_glEnable)resolve_gl_symbol("glEnable");
    pfn_glDisable = (fn_glDisable)resolve_gl_symbol("glDisable");
    pfn_glBlendFunc = (fn_glBlendFunc)resolve_gl_symbol("glBlendFunc");
    pfn_glViewport = (fn_glViewport)resolve_gl_symbol("glViewport");
    pfn_glBindBuffer = (fn_glBindBuffer)resolve_gl_symbol("glBindBuffer");
    pfn_glGenBuffers = (fn_glGenBuffers)resolve_gl_symbol("glGenBuffers");
    pfn_glDeleteBuffers = (fn_glDeleteBuffers)resolve_gl_symbol("glDeleteBuffers");
    pfn_glBufferData = (fn_glBufferData)resolve_gl_symbol("glBufferData");
    pfn_glBindVertexArray = (fn_glBindVertexArray)resolve_gl_symbol("glBindVertexArray");
    if (!pfn_glBindVertexArray)
        pfn_glBindVertexArray = (fn_glBindVertexArray)resolve_gl_symbol("glBindVertexArrayOES");
    pfn_glGenVertexArrays = (fn_glGenVertexArrays)resolve_gl_symbol("glGenVertexArrays");
    if (!pfn_glGenVertexArrays)
        pfn_glGenVertexArrays = (fn_glGenVertexArrays)resolve_gl_symbol("glGenVertexArraysOES");
    pfn_glDeleteVertexArrays =
        (fn_glDeleteVertexArrays)resolve_gl_symbol("glDeleteVertexArrays");
    if (!pfn_glDeleteVertexArrays)
        pfn_glDeleteVertexArrays =
            (fn_glDeleteVertexArrays)resolve_gl_symbol("glDeleteVertexArraysOES");
    pfn_glEnableVertexAttribArray =
        (fn_glEnableVertexAttribArray)resolve_gl_symbol("glEnableVertexAttribArray");
    pfn_glDisableVertexAttribArray =
        (fn_glDisableVertexAttribArray)resolve_gl_symbol("glDisableVertexAttribArray");
    pfn_glVertexAttribPointer =
        (fn_glVertexAttribPointer)resolve_gl_symbol("glVertexAttribPointer");
    pfn_glDrawArrays = (fn_glDrawArrays)resolve_gl_symbol("glDrawArrays");
    pfn_glGetIntegerv = (fn_glGetIntegerv)resolve_gl_symbol("glGetIntegerv");
    pfn_glIsEnabled = (fn_glIsEnabled)resolve_gl_symbol("glIsEnabled");

    gl_funcs_ok = pfn_glCreateShader && pfn_glShaderSource && pfn_glCompileShader &&
                  pfn_glGetShaderiv && pfn_glDeleteShader && pfn_glCreateProgram &&
                  pfn_glAttachShader && pfn_glLinkProgram && pfn_glGetProgramiv &&
                  pfn_glDeleteProgram && pfn_glUseProgram && pfn_glGetAttribLocation &&
                  pfn_glGetUniformLocation && pfn_glUniform1i && pfn_glGenTextures &&
                  pfn_glDeleteTextures && pfn_glBindTexture && pfn_glTexParameteri &&
                  pfn_glTexImage2D && pfn_glTexSubImage2D && pfn_glActiveTexture &&
                  pfn_glEnable && pfn_glDisable && pfn_glBlendFunc && pfn_glViewport &&
                  pfn_glBindBuffer && pfn_glGenBuffers && pfn_glDeleteBuffers &&
                  pfn_glBufferData && pfn_glBindVertexArray && pfn_glGenVertexArrays &&
                  pfn_glDeleteVertexArrays && pfn_glEnableVertexAttribArray &&
                  pfn_glDisableVertexAttribArray && pfn_glVertexAttribPointer &&
                  pfn_glDrawArrays && pfn_glGetIntegerv && pfn_glIsEnabled;
}

static void gl_reset_resources(void) {
    gl_context = NULL;
    gl_program = 0;
    gl_vbo = 0;
    gl_vao = 0;
    gl_attr_position = -1;
    gl_attr_texcoord = -1;
    gl_uniform_texture = -1;
    gl_program_ready = 0;
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++) {
        gl_slot_texture[i] = 0;
        gl_slot_tex_w[i] = 0;
        gl_slot_tex_h[i] = 0;
        gl_slot_texture_frame_id[i] = UINT32_MAX;
    }
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
    gl_reset_resources();

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

    /* Cache is already refreshed by collect_active_slots().  Do NOT call
       slot_refresh_cache() here — it could update position/size between
       the save_slot_background() and draw_slot() passes, causing the
       saved background region to not match the drawn pill region. */

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

/* ── OpenGL overlay drawing for MinArch ────────────────────────── */

typedef struct {
    GLint current_program;
    GLint array_buffer;
    GLint vertex_array_binding;
    GLint active_texture;
    GLint texture_binding_2d;
    GLint viewport[4];
    GLint blend_src_rgb;
    GLint blend_dst_rgb;
    GLboolean blend_enabled;
    GLboolean depth_test_enabled;
    GLboolean cull_face_enabled;
    GLboolean scissor_test_enabled;
} gl_saved_state_t;

static const GLchar *overlay_vertex_shader_src =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main(void) {\n"
    "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "  v_texcoord = a_texcoord;\n"
    "}\n";

static const GLchar *overlay_fragment_shader_src =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_texture;\n"
    "varying vec2 v_texcoord;\n"
    "void main(void) {\n"
    "  gl_FragColor = texture2D(u_texture, v_texcoord);\n"
    "}\n";

static GLuint gl_compile_shader(GLenum type, const GLchar *source) {
    GLuint shader;
    GLint ok = 0;

    if (!source)
        return 0;

    shader = pfn_glCreateShader(type);
    if (!shader)
        return 0;

    pfn_glShaderSource(shader, 1, &source, NULL);
    pfn_glCompileShader(shader);
    pfn_glGetShaderiv(shader, VR_GL_COMPILE_STATUS, &ok);
    if (!ok) {
        pfn_glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int ensure_gl_program(void) {
    GLuint vs, fs, program;
    GLint ok = 0;

    if (!gl_funcs_ok)
        return 0;
    if (gl_program_ready)
        return 1;

    vs = gl_compile_shader(VR_GL_VERTEX_SHADER, overlay_vertex_shader_src);
    if (!vs)
        return 0;

    fs = gl_compile_shader(VR_GL_FRAGMENT_SHADER, overlay_fragment_shader_src);
    if (!fs) {
        pfn_glDeleteShader(vs);
        return 0;
    }

    program = pfn_glCreateProgram();
    if (!program) {
        pfn_glDeleteShader(vs);
        pfn_glDeleteShader(fs);
        return 0;
    }

    pfn_glAttachShader(program, vs);
    pfn_glAttachShader(program, fs);
    pfn_glLinkProgram(program);
    pfn_glGetProgramiv(program, VR_GL_LINK_STATUS, &ok);
    pfn_glDeleteShader(vs);
    pfn_glDeleteShader(fs);
    if (!ok) {
        pfn_glDeleteProgram(program);
        return 0;
    }

    gl_attr_position = pfn_glGetAttribLocation(program, "a_position");
    gl_attr_texcoord = pfn_glGetAttribLocation(program, "a_texcoord");
    gl_uniform_texture = pfn_glGetUniformLocation(program, "u_texture");
    if (gl_attr_position < 0 || gl_attr_texcoord < 0 || gl_uniform_texture < 0) {
        pfn_glDeleteProgram(program);
        return 0;
    }

    pfn_glGenBuffers(1, &gl_vbo);
    if (!gl_vbo) {
        pfn_glDeleteProgram(program);
        return 0;
    }

    pfn_glGenVertexArrays(1, &gl_vao);
    if (!gl_vao) {
        pfn_glDeleteBuffers(1, &gl_vbo);
        gl_vbo = 0;
        pfn_glDeleteProgram(program);
        return 0;
    }

    gl_program = program;
    gl_program_ready = 1;
    return 1;
}

static int ensure_gl_context_state(void) {
    void *current_context;

    if (!pfn_SDL_GL_GetProcAddress)
        return 0;
    if (!gl_funcs_ok)
        init_gl_funcs();
    if (!gl_funcs_ok)
        return 0;

    current_context = pfn_SDL_GL_GetCurrentContext ?
        pfn_SDL_GL_GetCurrentContext() : NULL;
    if (current_context != gl_context) {
        gl_context = current_context;
        gl_program = 0;
        gl_vbo = 0;
        gl_vao = 0;
        gl_attr_position = -1;
        gl_attr_texcoord = -1;
        gl_uniform_texture = -1;
        gl_program_ready = 0;
        for (int i = 0; i < VARNISH_MAX_SLOTS; i++) {
            gl_slot_texture[i] = 0;
            gl_slot_tex_w[i] = 0;
            gl_slot_tex_h[i] = 0;
            gl_slot_texture_frame_id[i] = UINT32_MAX;
        }
    }

    return ensure_gl_program();
}

static void gl_save_state(gl_saved_state_t *state) {
    if (!state)
        return;

    memset(state, 0, sizeof(*state));
    pfn_glGetIntegerv(VR_GL_CURRENT_PROGRAM, &state->current_program);
    pfn_glGetIntegerv(VR_GL_ARRAY_BUFFER_BINDING, &state->array_buffer);
    pfn_glGetIntegerv(VR_GL_VERTEX_ARRAY_BINDING, &state->vertex_array_binding);
    pfn_glGetIntegerv(VR_GL_ACTIVE_TEXTURE, &state->active_texture);
    pfn_glActiveTexture(VR_GL_TEXTURE0);
    pfn_glGetIntegerv(VR_GL_TEXTURE_BINDING_2D, &state->texture_binding_2d);
    pfn_glGetIntegerv(VR_GL_VIEWPORT, state->viewport);
    pfn_glGetIntegerv(VR_GL_BLEND_SRC_RGB, &state->blend_src_rgb);
    pfn_glGetIntegerv(VR_GL_BLEND_DST_RGB, &state->blend_dst_rgb);
    state->blend_enabled = pfn_glIsEnabled(VR_GL_BLEND);
    state->depth_test_enabled = pfn_glIsEnabled(VR_GL_DEPTH_TEST);
    state->cull_face_enabled = pfn_glIsEnabled(VR_GL_CULL_FACE);
    state->scissor_test_enabled = pfn_glIsEnabled(VR_GL_SCISSOR_TEST);
}

static void gl_restore_state(const gl_saved_state_t *state) {
    if (!state)
        return;

    pfn_glUseProgram((GLuint)state->current_program);
    pfn_glBindVertexArray((GLuint)state->vertex_array_binding);
    pfn_glBindBuffer(VR_GL_ARRAY_BUFFER, (GLuint)state->array_buffer);
    if (state->blend_enabled)
        pfn_glEnable(VR_GL_BLEND);
    else
        pfn_glDisable(VR_GL_BLEND);
    if (state->depth_test_enabled)
        pfn_glEnable(VR_GL_DEPTH_TEST);
    else
        pfn_glDisable(VR_GL_DEPTH_TEST);
    if (state->cull_face_enabled)
        pfn_glEnable(VR_GL_CULL_FACE);
    else
        pfn_glDisable(VR_GL_CULL_FACE);
    if (state->scissor_test_enabled)
        pfn_glEnable(VR_GL_SCISSOR_TEST);
    else
        pfn_glDisable(VR_GL_SCISSOR_TEST);
    pfn_glBlendFunc((GLenum)state->blend_src_rgb, (GLenum)state->blend_dst_rgb);
    pfn_glActiveTexture(VR_GL_TEXTURE0);
    pfn_glBindTexture(VR_GL_TEXTURE_2D, (GLuint)state->texture_binding_2d);
    pfn_glActiveTexture((GLenum)state->active_texture);
    pfn_glViewport(state->viewport[0], state->viewport[1],
                   state->viewport[2], state->viewport[3]);
}

static void gl_convert_slot_pixels(int idx) {
    const uint32_t *src = slot_cached_pixels[idx];
    uint8_t *dst = gl_slot_rgba[idx];
    int pixel_count = slot_cached_w[idx] * slot_cached_h[idx];

    for (int i = 0; i < pixel_count; i++) {
        uint32_t pixel = src[i];
        dst[i * 4 + 0] = (uint8_t)((pixel >> 16) & 0xffu);
        dst[i * 4 + 1] = (uint8_t)((pixel >> 8) & 0xffu);
        dst[i * 4 + 2] = (uint8_t)(pixel & 0xffu);
        dst[i * 4 + 3] = (uint8_t)((pixel >> 24) & 0xffu);
    }
}

static int gl_ensure_slot_texture(int idx) {
    int recreate = 0;

    if (!slot_cached_active[idx] || slot_cached_w[idx] <= 0 || slot_cached_h[idx] <= 0)
        return 0;

    if (gl_slot_texture[idx] &&
        (gl_slot_tex_w[idx] != slot_cached_w[idx] ||
         gl_slot_tex_h[idx] != slot_cached_h[idx])) {
        pfn_glDeleteTextures(1, &gl_slot_texture[idx]);
        gl_slot_texture[idx] = 0;
        gl_slot_texture_frame_id[idx] = UINT32_MAX;
    }

    if (!gl_slot_texture[idx]) {
        pfn_glGenTextures(1, &gl_slot_texture[idx]);
        if (!gl_slot_texture[idx])
            return 0;
        recreate = 1;
        gl_slot_tex_w[idx] = slot_cached_w[idx];
        gl_slot_tex_h[idx] = slot_cached_h[idx];
        gl_slot_texture_frame_id[idx] = UINT32_MAX;
    }

    pfn_glBindTexture(VR_GL_TEXTURE_2D, gl_slot_texture[idx]);
    pfn_glTexParameteri(VR_GL_TEXTURE_2D, VR_GL_TEXTURE_MIN_FILTER, VR_GL_LINEAR);
    pfn_glTexParameteri(VR_GL_TEXTURE_2D, VR_GL_TEXTURE_MAG_FILTER, VR_GL_LINEAR);
    pfn_glTexParameteri(VR_GL_TEXTURE_2D, VR_GL_TEXTURE_WRAP_S, VR_GL_CLAMP_TO_EDGE);
    pfn_glTexParameteri(VR_GL_TEXTURE_2D, VR_GL_TEXTURE_WRAP_T, VR_GL_CLAMP_TO_EDGE);

    if (gl_slot_texture_frame_id[idx] != slot_cached_frame_id[idx]) {
        gl_convert_slot_pixels(idx);
        if (recreate) {
            pfn_glTexImage2D(VR_GL_TEXTURE_2D, 0, (GLint)VR_GL_RGBA,
                             slot_cached_w[idx], slot_cached_h[idx], 0,
                             VR_GL_RGBA, VR_GL_UNSIGNED_BYTE, gl_slot_rgba[idx]);
        } else {
            pfn_glTexSubImage2D(VR_GL_TEXTURE_2D, 0, 0, 0,
                                slot_cached_w[idx], slot_cached_h[idx],
                                VR_GL_RGBA, VR_GL_UNSIGNED_BYTE, gl_slot_rgba[idx]);
        }
        gl_slot_texture_frame_id[idx] = slot_cached_frame_id[idx];
    }

    return 1;
}

static int draw_all_gl_overlays(void *window) {
    int order[VARNISH_MAX_SLOTS];
    int count;
    int drawable_w = 0;
    int drawable_h = 0;
    gl_saved_state_t saved_state;

    count = collect_active_slots(order);
    if (count <= 0)
        return 0;
    if (!ensure_gl_context_state())
        return 0;

    if (pfn_SDL_GL_GetDrawableSize)
        pfn_SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
    if (drawable_w <= 0 || drawable_h <= 0) {
        if (!shm || !shm_ok)
            return 0;
        drawable_w = shm->fb_width;
        drawable_h = shm->fb_height;
    }
    if (drawable_w <= 0 || drawable_h <= 0)
        return 0;

    gl_save_state(&saved_state);

    pfn_glUseProgram(gl_program);
    pfn_glActiveTexture(VR_GL_TEXTURE0);
    pfn_glUniform1i(gl_uniform_texture, 0);
    pfn_glBindVertexArray(gl_vao);
    pfn_glBindBuffer(VR_GL_ARRAY_BUFFER, gl_vbo);
    pfn_glEnableVertexAttribArray((GLuint)gl_attr_position);
    pfn_glEnableVertexAttribArray((GLuint)gl_attr_texcoord);
    pfn_glVertexAttribPointer((GLuint)gl_attr_position, 2, VR_GL_FLOAT,
                              VR_GL_FALSE, (GLsizei)(4 * sizeof(GLfloat)),
                              (const void *)0);
    pfn_glVertexAttribPointer((GLuint)gl_attr_texcoord, 2, VR_GL_FLOAT,
                              VR_GL_FALSE, (GLsizei)(4 * sizeof(GLfloat)),
                              (const void *)(uintptr_t)(2u * sizeof(GLfloat)));
    pfn_glViewport(0, 0, drawable_w, drawable_h);
    pfn_glDisable(VR_GL_DEPTH_TEST);
    pfn_glDisable(VR_GL_CULL_FACE);
    pfn_glDisable(VR_GL_SCISSOR_TEST);
    pfn_glEnable(VR_GL_BLEND);
    pfn_glBlendFunc(VR_GL_SRC_ALPHA, VR_GL_ONE_MINUS_SRC_ALPHA);

    for (int i = 0; i < count; i++) {
        int idx = order[i];
        GLfloat vertices[16];
        float left;
        float right;
        float top;
        float bottom;

        if (!gl_ensure_slot_texture(idx))
            continue;

        left = ((float)slot_cached_x[idx] / (float)drawable_w) * 2.0f - 1.0f;
        right = ((float)(slot_cached_x[idx] + slot_cached_w[idx]) / (float)drawable_w) * 2.0f - 1.0f;
        top = 1.0f - ((float)slot_cached_y[idx] / (float)drawable_h) * 2.0f;
        bottom = 1.0f - ((float)(slot_cached_y[idx] + slot_cached_h[idx]) / (float)drawable_h) * 2.0f;

        vertices[0] = left;   vertices[1] = top;    vertices[2] = 0.0f; vertices[3] = 0.0f;
        vertices[4] = right;  vertices[5] = top;    vertices[6] = 1.0f; vertices[7] = 0.0f;
        vertices[8] = left;   vertices[9] = bottom; vertices[10] = 0.0f; vertices[11] = 1.0f;
        vertices[12] = right; vertices[13] = bottom; vertices[14] = 1.0f; vertices[15] = 1.0f;

        pfn_glBindTexture(VR_GL_TEXTURE_2D, gl_slot_texture[idx]);
        pfn_glBufferData(VR_GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(vertices),
                         vertices, VR_GL_STATIC_DRAW);
        pfn_glDrawArrays(VR_GL_TRIANGLE_STRIP, 0, 4);
    }

    gl_restore_state(&saved_state);
    return count;
}

/* ── Per-slot background save / restore ────────────────────────── */

static void save_slot_background(void *renderer, int idx) {
    typedef struct { int x, y, w, h; } SDL_Rect;
    SDL_Rect rect;

    if (!pfn_RenderReadPixels) return;

    rect.x = slot_cached_x[idx];
    rect.y = slot_cached_y[idx];
    rect.w = slot_cached_w[idx];
    rect.h = slot_cached_h[idx];

    if (rect.w <= 0 || rect.h <= 0 ||
        rect.w > VARNISH_SLOT_MAX_W || rect.h > VARNISH_SLOT_MAX_H)
        return;

    if (pfn_RenderReadPixels(renderer, &rect, VR_SDL_PIXELFORMAT_ARGB8888,
                              slot_save_pixels[idx],
                              rect.w * (int)sizeof(uint32_t)) != 0)
        return;

    /* Force alpha=255 on all saved pixels.  Some renderers (framebuffer-
       backed, no alpha channel) return alpha=0 from SDL_RenderReadPixels.
       Without this, restored backgrounds become transparent-black, causing
       a visible black rectangle around pills during idle presents. */
    {
        int pixel_count = rect.w * rect.h;
        for (int i = 0; i < pixel_count; i++)
            slot_save_pixels[idx][i] |= 0xFF000000u;
    }

    /* Recreate save texture if renderer or dimensions changed */
    if (slot_save_texture[idx] &&
        (slot_save_renderer[idx] != renderer ||
         slot_save_w[idx] != rect.w || slot_save_h[idx] != rect.h)) {
        pfn_DestroyTexture(slot_save_texture[idx]);
        slot_save_texture[idx] = NULL;
    }

    if (!slot_save_texture[idx]) {
        if (!pfn_CreateTexture) return;
        slot_save_texture[idx] = pfn_CreateTexture(
            renderer, VR_SDL_PIXELFORMAT_ARGB8888,
            VR_SDL_TEXTUREACCESS_STREAMING, rect.w, rect.h);
        if (!slot_save_texture[idx]) return;
        slot_save_renderer[idx] = renderer;
    }

    pfn_UpdateTexture(slot_save_texture[idx], NULL, slot_save_pixels[idx],
                      rect.w * (int)sizeof(uint32_t));

    slot_save_x[idx] = rect.x;
    slot_save_y[idx] = rect.y;
    slot_save_w[idx] = rect.w;
    slot_save_h[idx] = rect.h;
    slot_save_valid[idx] = 1;
}

static void restore_slot_background(void *renderer, int idx) {
    typedef struct { int x, y, w, h; } SDL_Rect;
    SDL_Rect dst;

    if (!slot_save_valid[idx] || !slot_save_texture[idx] || !pfn_RenderCopy)
        return;
    if (slot_save_renderer[idx] != renderer)
        return;

    dst.x = slot_save_x[idx];
    dst.y = slot_save_y[idx];
    dst.w = slot_save_w[idx];
    dst.h = slot_save_h[idx];
    pfn_RenderCopy(renderer, slot_save_texture[idx], NULL, &dst);
}

static void restore_all_backgrounds(void *renderer) {
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++)
        restore_slot_background(renderer, i);
}

/* ── Full frame save / restore for idle-present ────────────────── */

static void save_full_frame(void *renderer) {
    int w, h, pixel_count;

    if (!pfn_RenderReadPixels) return;
    if (!shm || !shm_ok) return;

    w = shm->fb_width;
    h = shm->fb_height;
    if (w <= 0 || h <= 0 || w > FULL_FRAME_MAX_W || h > FULL_FRAME_MAX_H)
        return;

    if (pfn_RenderReadPixels(renderer, NULL, VR_SDL_PIXELFORMAT_ARGB8888,
                              full_frame_pixels, w * (int)sizeof(uint32_t)) != 0)
        return;

    pixel_count = w * h;
    for (int i = 0; i < pixel_count; i++)
        full_frame_pixels[i] |= 0xFF000000u;

    if (full_frame_texture &&
        (full_frame_renderer != renderer ||
         full_frame_w != w || full_frame_h != h)) {
        pfn_DestroyTexture(full_frame_texture);
        full_frame_texture = NULL;
    }

    if (!full_frame_texture) {
        if (!pfn_CreateTexture) return;
        full_frame_texture = pfn_CreateTexture(
            renderer, VR_SDL_PIXELFORMAT_ARGB8888,
            VR_SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!full_frame_texture) return;
        full_frame_renderer = renderer;
    }

    pfn_UpdateTexture(full_frame_texture, NULL, full_frame_pixels,
                      w * (int)sizeof(uint32_t));

    full_frame_w = w;
    full_frame_h = h;
    full_frame_valid = 1;
}

static void restore_full_frame(void *renderer) {
    if (!full_frame_valid || !full_frame_texture || !pfn_RenderCopy)
        return;
    if (full_frame_renderer != renderer)
        return;
    pfn_RenderCopy(renderer, full_frame_texture, NULL, NULL);
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

    /*
     * Restore the full clean frame if available (covers entire screen,
     * avoids black bars from undefined back-buffer content).  Fall back
     * to per-slot background restoration when no full frame was saved.
     * Then composite current overlays fresh.
     */
    force_present_guard = 1;
    if (full_frame_valid)
        restore_full_frame(last_renderer);
    else
        restore_all_backgrounds(last_renderer);
    if (__builtin_expect(sdl_funcs_ok, 1)) {
        int order[VARNISH_MAX_SLOTS];
        int count = collect_active_slots(order);
        for (i = 0; i < count; i++) {
            if (!slot_save_valid[order[i]])
                save_slot_background(last_renderer, order[i]);
        }
        for (i = 0; i < count; i++)
            draw_slot(last_renderer, order[i]);
    }
    real_present(last_renderer);
    for (i = 0; i < VARNISH_MAX_SLOTS; i++)
        last_present_frame_ids[i] = slot_cached_frame_id[i];
    force_present_guard = 0;
}

/* ── SDL_RenderPresent interposition ────────────────────────────── */

void SDL_RenderPresent(void *renderer) {
    int order[VARNISH_MAX_SLOTS];
    int count;

    if (__builtin_expect(!real_present, 0)) {
        real_present = (fn_SDL_RenderPresent)dlsym(RTLD_NEXT, "SDL_RenderPresent");
        if (!real_present) return;
        init_sdl_funcs();
    }

    last_renderer = renderer;
    render_tid = current_tid();

    if (__builtin_expect(sdl_funcs_ok, 1)) {
        count = collect_active_slots(order);

        if (count > 0) {
            /* Save the full clean frame for idle-present restoration.
             * This ensures we can repaint the entire screen when pills
             * disappear during idle (back buffer is undefined after swap). */
            save_full_frame(renderer);
            /* Save the clean region behind each pill BEFORE drawing.
             * Per-slot region readback: ~72KB per pill vs 3MB full frame. */
            for (int i = 0; i < count; i++)
                save_slot_background(renderer, order[i]);
            for (int i = 0; i < count; i++)
                draw_slot(renderer, order[i]);
        } else {
            /* No overlays — invalidate all stale saves */
            for (int i = 0; i < VARNISH_MAX_SLOTS; i++)
                slot_save_valid[i] = 0;
        }
    }

    real_present(renderer);
    for (int i = 0; i < VARNISH_MAX_SLOTS; i++)
        last_present_frame_ids[i] = slot_cached_frame_id[i];
}

void SDL_GL_SwapWindow(void *window) {
    if (__builtin_expect(!real_gl_swap, 0)) {
        real_gl_swap = (fn_SDL_GL_SwapWindow)resolve_symbol_any("SDL_GL_SwapWindow");
        if (!real_gl_swap)
            return;
        init_sdl_funcs();
    }

    draw_all_gl_overlays(window);
    real_gl_swap(window);
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
