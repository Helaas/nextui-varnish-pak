/*
 * overlay.c — Pill rendering for Varnish overlay system.
 *
 * Renders text pills as ARGB8888 pixel buffers suitable for writing into
 * shared memory slots.  Uses SDL2 + SDL_ttf for off-screen rendering
 * (no window needed).  Reads theme colors from NextUI settings.
 */

#include "overlay.h"
#include "device.h"
#include "strutil.h"
#include "varnish_shm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#endif

/* ── Constants ─────────────────────────────────────────────────── */

#define OVERLAY_THEME_JSON_MAX           4096
#define OVERLAY_DEFAULT_ACCENT_R          155
#define OVERLAY_DEFAULT_ACCENT_G           34
#define OVERLAY_DEFAULT_ACCENT_B           87
#define OVERLAY_DEFAULT_HINT_R            255
#define OVERLAY_DEFAULT_HINT_G            255
#define OVERLAY_DEFAULT_HINT_B            255
#define OVERLAY_PILL_SIZE                  30
#define OVERLAY_PILL_PADDING               10

/* ── State ─────────────────────────────────────────────────────── */

static int ol_fb_width;
static int ol_fb_height;
static int ol_device_scale  = 2;
static int ol_device_padding = 10;

#ifdef __linux__

static SDL_Color ol_accent = {
    OVERLAY_DEFAULT_ACCENT_R,
    OVERLAY_DEFAULT_ACCENT_G,
    OVERLAY_DEFAULT_ACCENT_B,
    255
};
static SDL_Color ol_hint = {
    OVERLAY_DEFAULT_HINT_R,
    OVERLAY_DEFAULT_HINT_G,
    OVERLAY_DEFAULT_HINT_B,
    255
};
static TTF_Font *ol_font;
static SDL_Surface *ol_cap_assets;

/* ── Theme / settings helpers ──────────────────────────────────── */

static SDL_Color color_from_hex(const char *hex, SDL_Color fallback) {
    SDL_Color c = fallback;
    unsigned long value;

    if (!hex || !hex[0]) return c;
    if (hex[0] == '#') hex++;
    else if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;

    value = strtoul(hex, NULL, 16);
    c.r = (Uint8)((value >> 16) & 0xFF);
    c.g = (Uint8)((value >> 8) & 0xFF);
    c.b = (Uint8)(value & 0xFF);
    c.a = 255;
    return c;
}

static const char *nextui_settings_path(char *buf, size_t buf_size) {
#ifndef PLATFORM_MAC
    char shared[256];
    device_get_shared_userdata_path(shared, sizeof(shared));
    if (!shared[0]) return NULL;
    if (path_join(buf, buf_size, shared, "minuisettings.txt") != 0)
        return NULL;
    return buf;
#else
    const char *path = getenv("AP_MINUI_SETTINGS_PATH");
    (void)buf; (void)buf_size;
    return (path && path[0]) ? path : NULL;
#endif
}

static int settings_copy_hex_string(const char *key, char *out, size_t out_size) {
    char settings_path[256];
    const char *path = nextui_settings_path(settings_path, sizeof(settings_path));
    FILE *fp;
    char line[256];
    size_t key_len;

    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!path || !path[0]) return 0;

    fp = fopen(path, "r");
    if (!fp) return 0;

    key_len = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        unsigned int value = 0;
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '='
            && sscanf(line + key_len + 1, "%x", &value) == 1) {
            snprintf(out, out_size, "0x%06X", value & 0xFFFFFF);
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int read_nextui_setting_int(const char *key, int default_val) {
    char settings_path[256];
    const char *path = nextui_settings_path(settings_path, sizeof(settings_path));
    FILE *fp;
    char line[256];
    size_t key_len;

    if (!path || !path[0]) return default_val;

    fp = fopen(path, "r");
    if (!fp) return default_val;

    key_len = strlen(key);
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            int value = 0;
            if (sscanf(line + key_len + 1, "%d", &value) == 1) {
                fclose(fp);
                return value;
            }
        }
    }

    fclose(fp);
    return default_val;
}

static const char *json_find_string(const char *json, const char *key) {
    static char value_buf[256];
    char search[128];
    const char *pos;
    int i = 0;

    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return NULL;

    pos += strlen(search);
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;
    if (*pos != '"') return NULL;
    pos++;

    while (*pos && *pos != '"' && i < (int)sizeof(value_buf) - 1)
        value_buf[i++] = *pos++;
    value_buf[i] = '\0';
    return value_buf;
}

static int json_copy_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *value;
    size_t n;

    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    value = json_find_string(json, key);
    if (!value) return 0;

    n = strlen(value);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, value, n);
    out[n] = '\0';
    return 1;
}

static int load_theme_from_json(const char *json) {
    char accent_buf[32] = {0};
    char hint_buf[32] = {0};
    char text_buf[32] = {0};

    if (!json || !json[0]) return -1;

    if (json_copy_string(json, "color2", accent_buf, sizeof(accent_buf)))
        ol_accent = color_from_hex(accent_buf, ol_accent);

    if (json_copy_string(json, "color6", hint_buf, sizeof(hint_buf)))
        ol_hint = color_from_hex(hint_buf, ol_hint);
    else if (json_copy_string(json, "color4", text_buf, sizeof(text_buf)))
        ol_hint = color_from_hex(text_buf, ol_hint);

    return 0;
}

static int load_theme_from_device_nextval(void) {
#ifndef PLATFORM_MAC
    const char *nextval_path = NULL;
    const char *system_path_env = getenv("SYSTEM_PATH");
    char nextval_buf[256] = {0};
    char json[OVERLAY_THEME_JSON_MAX];
    FILE *fp;
    size_t total = 0;

    if (system_path_env && system_path_env[0]) {
        snprintf(nextval_buf, sizeof(nextval_buf), "%s/bin/nextval.elf", system_path_env);
        if (access(nextval_buf, X_OK) == 0) nextval_path = nextval_buf;
    }

    if (!nextval_path) {
#if defined(PLATFORM_TG5040)
        if (access("/mnt/SDCARD/.system/tg5040/bin/nextval.elf", X_OK) == 0)
            nextval_path = "/mnt/SDCARD/.system/tg5040/bin/nextval.elf";
#elif defined(PLATFORM_TG5050)
        if (access("/mnt/SDCARD/.system/tg5050/bin/nextval.elf", X_OK) == 0)
            nextval_path = "/mnt/SDCARD/.system/tg5050/bin/nextval.elf";
#elif defined(PLATFORM_MY355)
        if (access("/mnt/SDCARD/.system/my355/bin/nextval.elf", X_OK) == 0)
            nextval_path = "/mnt/SDCARD/.system/my355/bin/nextval.elf";
#endif
    }

    if (!nextval_path) return -1;

    fp = popen(nextval_path, "r");
    if (!fp) return -1;

    while (total < sizeof(json) - 1) {
        size_t n = fread(json + total, 1, sizeof(json) - 1 - total, fp);
        if (n == 0) break;
        total += n;
    }
    json[total] = '\0';
    {
        int status = pclose(fp);
        if (status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            fprintf(stderr, "varnish: nextval.elf exited with error (status=%d)\n", status);
    }

    if (total == 0) return -1;
    return load_theme_from_json(json);
#else
    return -1;
#endif
}

static void load_overlay_theme(void) {
    const char *path = getenv("AP_NEXTVAL_PATH");
    char accent_buf[32] = {0};
    char hint_buf[32] = {0};

    ol_accent.r = OVERLAY_DEFAULT_ACCENT_R;
    ol_accent.g = OVERLAY_DEFAULT_ACCENT_G;
    ol_accent.b = OVERLAY_DEFAULT_ACCENT_B;
    ol_accent.a = 255;
    ol_hint.r = OVERLAY_DEFAULT_HINT_R;
    ol_hint.g = OVERLAY_DEFAULT_HINT_G;
    ol_hint.b = OVERLAY_DEFAULT_HINT_B;
    ol_hint.a = 255;

    if (settings_copy_hex_string("color2", accent_buf, sizeof(accent_buf)))
        ol_accent = color_from_hex(accent_buf, ol_accent);
    if (settings_copy_hex_string("color6", hint_buf, sizeof(hint_buf)))
        ol_hint = color_from_hex(hint_buf, ol_hint);

    if (path && path[0]) {
        FILE *fp = fopen(path, "r");
        if (fp) {
            char json[OVERLAY_THEME_JSON_MAX];
            size_t nread = fread(json, 1, sizeof(json) - 1, fp);
            fclose(fp);
            if (nread > 0) {
                json[nread] = '\0';
                load_theme_from_json(json);
                return;
            }
        }
    }

    (void)load_theme_from_device_nextval();
}

/* ── Font resolution ───────────────────────────────────────────── */

static int overlay_font_size(void) {
    int size = 12 * ol_device_scale;
    return size < 8 ? 8 : size;
}

static const char *resolve_font_path(char *buf, size_t buf_size) {
    static const char * const search_paths[] = {
        "./font.ttf",
        "./res/font.ttf",
        "../res/font.ttf",
        "/mnt/SDCARD/.system/res/font1.ttf",
        "/mnt/SDCARD/.system/res/font2.ttf",
        "/mnt/SDCARD/.system/res/font.ttf",
        "/mnt/SDCARD/.system/tg5040/res/font.ttf",
        "/mnt/SDCARD/.system/my355/res/font.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        NULL,
    };
    int font_id;
    const char *sdcard;
    const char *font_name;

    font_id = read_nextui_setting_int("font", 1);
    font_name = (font_id == 1) ? "font1.ttf" : "font2.ttf";

    sdcard = getenv("SDCARD_PATH");
    if (!sdcard || !sdcard[0]) sdcard = "/mnt/SDCARD";
    snprintf(buf, buf_size, "%s/.system/res/%s", sdcard, font_name);
    if (access(buf, R_OK) == 0) return buf;

    for (int i = 0; search_paths[i]; i++) {
        if (access(search_paths[i], R_OK) == 0)
            return search_paths[i];
    }

    return NULL;
}

/* ── Cap asset loading ─────────────────────────────────────────── */

static const char *status_assets_dir(char *buf, size_t buf_size) {
    const char *dir = getenv("AP_STATUS_ASSETS_DIR");
    if (dir && dir[0]) return dir;

#ifndef PLATFORM_MAC
    {
        const char *sdcard = getenv("SDCARD_PATH");
        if (!sdcard || !sdcard[0]) sdcard = "/mnt/SDCARD";
        snprintf(buf, buf_size, "%s/.system/res", sdcard);
        return buf;
    }
#else
    (void)buf; (void)buf_size;
    return NULL;
#endif
}

/* ── Text fitting ──────────────────────────────────────────────── */

static size_t utf8_trim_boundary(const char *text, size_t len) {
    while (len > 0 && (((unsigned char)text[len] & 0xC0) == 0x80))
        len--;
    return len;
}

static void fit_text_to_width(const char *src, char *dst, size_t dst_size, int max_w) {
    static const char ellipsis[] = "...";
    int full_w = 0;
    int ellipsis_w = 0;
    char candidate[300];
    size_t len;

    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src || !src[0] || !ol_font || max_w <= 0) return;

    if (TTF_SizeUTF8(ol_font, src, &full_w, NULL) == 0 && full_w <= max_w) {
        str_copy_trunc(dst, dst_size, src);
        return;
    }

    if (TTF_SizeUTF8(ol_font, ellipsis, &ellipsis_w, NULL) < 0 || ellipsis_w > max_w)
        return;

    len = strlen(src);
    while (len > 0) {
        int candidate_w = 0;

        len = utf8_trim_boundary(src, len);
        memcpy(candidate, src, len);
        candidate[len] = '\0';
        strncat(candidate, ellipsis, sizeof(candidate) - strlen(candidate) - 1);

        if (TTF_SizeUTF8(ol_font, candidate, &candidate_w, NULL) == 0
            && candidate_w <= max_w) {
            str_copy_trunc(dst, dst_size, candidate);
            return;
        }

        if (len == 0) break;
        len--;
    }

    str_copy_trunc(dst, dst_size, ellipsis);
}

/* ── Surface drawing ───────────────────────────────────────────── */

static void put_surface_pixel(SDL_Surface *surface, int x, int y,
                              Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    Uint32 *row;

    if (!surface || x < 0 || y < 0 || x >= surface->w || y >= surface->h)
        return;

    row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
    row[x] = SDL_MapRGBA(surface->format, r, g, b, a);
}

static void draw_rounded_rect_surface(SDL_Surface *surface, int x0, int y0, int w, int h,
                                      int radius, SDL_Color color) {
    if (!surface || w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int draw = 1;

            if (px < radius && py < radius) {
                int dx = radius - px - 1;
                int dy = radius - py - 1;
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px >= w - radius && py < radius) {
                int dx = px - (w - radius);
                int dy = radius - py - 1;
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px < radius && py >= h - radius) {
                int dx = radius - px - 1;
                int dy = py - (h - radius);
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            } else if (px >= w - radius && py >= h - radius) {
                int dx = px - (w - radius);
                int dy = py - (h - radius);
                if (dx * dx + dy * dy > radius * radius) draw = 0;
            }

            if (draw)
                put_surface_pixel(surface, x0 + px, y0 + py,
                                  color.r, color.g, color.b, color.a);
        }
    }
}

static void blit_tinted_surface_region(SDL_Surface *src, const SDL_Rect *src_rect,
                                       SDL_Surface *dst, int dx, int dy, SDL_Color tint) {
    int src_locked = 0;
    int dst_locked = 0;

    if (!src || !src_rect || !dst) return;

    if (SDL_MUSTLOCK(src) && SDL_LockSurface(src) == 0) src_locked = 1;
    if (SDL_MUSTLOCK(dst) && SDL_LockSurface(dst) == 0) dst_locked = 1;

    for (int y = 0; y < src_rect->h; y++) {
        for (int x = 0; x < src_rect->w; x++) {
            Uint32 *row = (Uint32 *)((Uint8 *)src->pixels + (src_rect->y + y) * src->pitch);
            Uint32 pixel = row[src_rect->x + x];
            Uint8 sr, sg, sb, sa;

            SDL_GetRGBA(pixel, src->format, &sr, &sg, &sb, &sa);
            if (sa == 0) continue;

            put_surface_pixel(dst, dx + x, dy + y,
                              (Uint8)((sr * tint.r) / 255),
                              (Uint8)((sg * tint.g) / 255),
                              (Uint8)((sb * tint.b) / 255),
                              (Uint8)((sa * tint.a) / 255));
        }
    }

    if (src_locked) SDL_UnlockSurface(src);
    if (dst_locked) SDL_UnlockSurface(dst);
}

/* ── Layout computation ────────────────────────────────────────── */

typedef struct {
    int x, y, w, h;
    int text_x, text_y;
    char text[300];
    SDL_Surface *text_surface;
} overlay_layout_t;

static int build_layout(overlay_layout_t *layout, const char *text,
                        varnish_position_t position) {
    int pill_h;
    int inner_margin;
    int side_margin;
    int max_pill_w;
    int max_text_w;
    int font_h;

    if (!layout || ol_fb_width <= 0 || ol_fb_height <= 0 || !text || !text[0] || !ol_font)
        return -1;

    memset(layout, 0, sizeof(*layout));
    pill_h = OVERLAY_PILL_SIZE * ol_device_scale;
    if (pill_h > VARNISH_SLOT_MAX_H)
        pill_h = VARNISH_SLOT_MAX_H;
    inner_margin = OVERLAY_PILL_PADDING * ol_device_scale;
    side_margin = ol_device_padding * ol_device_scale;
    max_pill_w = ol_fb_width - 2 * side_margin - 2 * pill_h;
    if (max_pill_w < pill_h)
        max_pill_w = ol_fb_width - 2 * side_margin;
    if (max_pill_w > VARNISH_SLOT_MAX_W)
        max_pill_w = VARNISH_SLOT_MAX_W;
    if (max_pill_w <= 0) return -1;

    max_text_w = max_pill_w - inner_margin * 2;
    if (max_text_w <= 0) return -1;

    fit_text_to_width(text, layout->text, sizeof(layout->text), max_text_w);
    if (!layout->text[0]) return -1;

    layout->text_surface = TTF_RenderUTF8_Blended(ol_font, layout->text, ol_hint);
    if (!layout->text_surface) return -1;

    font_h = TTF_FontHeight(ol_font);
    SDL_SetSurfaceBlendMode(layout->text_surface, SDL_BLENDMODE_BLEND);
    layout->w = layout->text_surface->w + inner_margin * 2;
    layout->h = pill_h;

    /* Horizontal position */
    switch (position) {
    case VARNISH_POS_TOP_LEFT:
    case VARNISH_POS_BOTTOM_LEFT:
        layout->x = side_margin;
        break;
    case VARNISH_POS_TOP_RIGHT:
    case VARNISH_POS_BOTTOM_RIGHT:
        layout->x = ol_fb_width - layout->w - side_margin;
        break;
    default: /* center */
        layout->x = (ol_fb_width - layout->w) / 2;
        break;
    }

    /* Vertical position */
    switch (position) {
    case VARNISH_POS_TOP_LEFT:
    case VARNISH_POS_TOP_CENTER:
    case VARNISH_POS_TOP_RIGHT:
        layout->y = side_margin;
        break;
    default: /* bottom */
        layout->y = ol_fb_height - pill_h - side_margin;
        break;
    }

    layout->text_x = (layout->w - layout->text_surface->w) / 2;
    layout->text_y = (layout->h - font_h) / 2;

    if (layout->x < 0) layout->x = 0;
    if (layout->y < 0) layout->y = 0;
    if (layout->x + layout->w > ol_fb_width) layout->w = ol_fb_width - layout->x;
    if (layout->y + layout->h > ol_fb_height) layout->h = ol_fb_height - layout->y;

    return (layout->w > 0 && layout->h > 0) ? 0 : -1;
}

static void destroy_layout(overlay_layout_t *layout) {
    if (!layout) return;
    if (layout->text_surface) {
        SDL_FreeSurface(layout->text_surface);
        layout->text_surface = NULL;
    }
}

static SDL_Surface *render_overlay_surface(const overlay_layout_t *layout) {
    SDL_Surface *surface;
    int cap_w;
    int center_w;
    SDL_Rect text_dst;

    if (!layout || layout->w <= 0 || layout->h <= 0) return NULL;

    surface = SDL_CreateRGBSurfaceWithFormat(0, layout->w, layout->h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;

    SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, 0, 0, 0, 0));
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);

    cap_w = layout->h / 2;
    center_w = layout->w - 2 * cap_w;

    if (ol_cap_assets && ol_device_scale > 0) {
        SDL_Rect left_src = {
            1 * ol_device_scale,
            1 * ol_device_scale,
            15 * ol_device_scale,
            30 * ol_device_scale
        };
        SDL_Rect right_src = {
            16 * ol_device_scale,
            1 * ol_device_scale,
            15 * ol_device_scale,
            30 * ol_device_scale
        };

        if (center_w > 0) {
            SDL_Rect center = {cap_w, 0, center_w, layout->h};
            SDL_FillRect(surface, &center,
                         SDL_MapRGBA(surface->format,
                                     ol_accent.r, ol_accent.g,
                                     ol_accent.b, ol_accent.a));
        }

        blit_tinted_surface_region(ol_cap_assets, &left_src, surface, 0, 0, ol_accent);
        blit_tinted_surface_region(ol_cap_assets, &right_src, surface,
                                   layout->w - cap_w, 0, ol_accent);
    } else {
        draw_rounded_rect_surface(surface, 0, 0, layout->w, layout->h,
                                  layout->h / 2, ol_accent);
    }

    if (layout->text_surface) {
        text_dst.x = layout->text_x;
        text_dst.y = layout->text_y;
        text_dst.w = layout->text_surface->w;
        text_dst.h = layout->text_surface->h;
        SDL_BlitSurface(layout->text_surface, NULL, surface, &text_dst);
    }

    return surface;
}

/* ── Pixel extraction (SDL_Surface -> ARGB8888 buffer) ─────────── */

static int extract_pixels(SDL_Surface *surface, int w, int h, uint32_t *out_pixels) {
    int locked = 0;

    if (!surface || !out_pixels || w <= 0 || h <= 0) return -1;

    if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) == 0) locked = 1;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            Uint32 *src_row = (Uint32 *)((Uint8 *)surface->pixels + row * surface->pitch);
            Uint32 pixel = src_row[col];
            Uint8 r, g, b, a;

            SDL_GetRGBA(pixel, surface->format, &r, &g, &b, &a);
            /* ARGB8888: ((a<<24)|(r<<16)|(g<<8)|b) */
            out_pixels[row * w + col] =
                ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                ((uint32_t)g << 8)  |  (uint32_t)b;
        }
    }

    if (locked) SDL_UnlockSurface(surface);
    return 0;
}

#endif /* __linux__ */

/* ── Public API ────────────────────────────────────────────────── */

int overlay_parse_position(const char *str) {
    if (!str) return -1;
    if (strcmp(str, "top-left")      == 0) return VARNISH_POS_TOP_LEFT;
    if (strcmp(str, "top-center")    == 0) return VARNISH_POS_TOP_CENTER;
    if (strcmp(str, "top-right")     == 0) return VARNISH_POS_TOP_RIGHT;
    if (strcmp(str, "bottom-left")   == 0) return VARNISH_POS_BOTTOM_LEFT;
    if (strcmp(str, "bottom-center") == 0) return VARNISH_POS_BOTTOM_CENTER;
    if (strcmp(str, "bottom-right")  == 0) return VARNISH_POS_BOTTOM_RIGHT;
    return -1;
}

int overlay_init(int fb_width, int fb_height) {
#ifdef __linux__
    char font_path[512];
    const char *resolved_font;
    char assets_dir_buf[256];
    const char *assets_dir;
    char asset_path[512];

    ol_fb_width = fb_width;
    ol_fb_height = fb_height;
    ol_device_scale = device_get_scale(fb_width, fb_height);
    ol_device_padding = device_get_padding(fb_width, fb_height);

    load_overlay_theme();

    if (SDL_WasInit(0) == 0 && SDL_Init(0) < 0) {
        fprintf(stderr, "varnish: overlay: SDL init failed: %s\n", SDL_GetError());
        return -1;
    }

    if (!TTF_WasInit() && TTF_Init() < 0) {
        fprintf(stderr, "varnish: overlay: TTF init failed: %s\n", TTF_GetError());
        return -1;
    }

    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
        fprintf(stderr, "varnish: overlay: IMG init failed: %s\n", IMG_GetError());
    }

    resolved_font = resolve_font_path(font_path, sizeof(font_path));
    if (!resolved_font) {
        fprintf(stderr, "varnish: overlay: no font file found\n");
    } else {
        ol_font = TTF_OpenFont(resolved_font, overlay_font_size());
        if (!ol_font) {
            fprintf(stderr, "varnish: overlay: font load failed: %s\n", TTF_GetError());
        } else {
            TTF_SetFontStyle(ol_font, TTF_STYLE_BOLD);
        }
    }

    /* Load pill cap assets */
    assets_dir = status_assets_dir(assets_dir_buf, sizeof(assets_dir_buf));
    if (assets_dir && assets_dir[0]) {
        snprintf(asset_path, sizeof(asset_path), "%s/assets@%dx.png",
                 assets_dir, ol_device_scale);
        ol_cap_assets = IMG_Load(asset_path);
        if (ol_cap_assets) {
            SDL_Surface *converted = SDL_ConvertSurfaceFormat(
                ol_cap_assets, SDL_PIXELFORMAT_RGBA32, 0);
            if (converted) {
                SDL_FreeSurface(ol_cap_assets);
                ol_cap_assets = converted;
            }
            SDL_SetSurfaceBlendMode(ol_cap_assets, SDL_BLENDMODE_BLEND);
        }
    }

    fprintf(stderr, "varnish: overlay: init ok (fb %dx%d, scale %d)\n",
            fb_width, fb_height, ol_device_scale);
    return 0;
#else
    ol_fb_width = fb_width;
    ol_fb_height = fb_height;
    ol_device_scale = device_get_scale(fb_width, fb_height);
    ol_device_padding = device_get_padding(fb_width, fb_height);
    return 0;
#endif
}

int overlay_render_pill(const char *text, varnish_position_t position,
                        int *out_x, int *out_y, int *out_w, int *out_h,
                        uint32_t *out_pixels) {
#ifdef __linux__
    overlay_layout_t layout;
    SDL_Surface *surface;
    int ret;

    if (build_layout(&layout, text, position) < 0)
        return -1;

    surface = render_overlay_surface(&layout);
    if (!surface) {
        destroy_layout(&layout);
        return -1;
    }

    ret = extract_pixels(surface, layout.w, layout.h, out_pixels);
    if (ret == 0) {
        if (out_x) *out_x = layout.x;
        if (out_y) *out_y = layout.y;
        if (out_w) *out_w = layout.w;
        if (out_h) *out_h = layout.h;
    }

    SDL_FreeSurface(surface);
    destroy_layout(&layout);
    return ret;
#else
    (void)text; (void)position;
    (void)out_x; (void)out_y; (void)out_w; (void)out_h; (void)out_pixels;
    return -1;
#endif
}

void overlay_cleanup(void) {
#ifdef __linux__
    if (ol_font) {
        TTF_CloseFont(ol_font);
        ol_font = NULL;
    }
    if (ol_cap_assets) {
        SDL_FreeSurface(ol_cap_assets);
        ol_cap_assets = NULL;
    }
#endif
}
