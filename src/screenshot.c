/*
 * screenshot.c — Framebuffer screenshot helpers.
 */

#include "screenshot.h"

#include "strutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#endif

#define SCREENSHOT_MAX_PATH 512
#define SCREENSHOT_DEFAULT_DIR "/mnt/SDCARD/Screenshots"

static int screenshot_build_candidate_path(const char *dir, time_t now,
                                           int suffix, char *out_path,
                                           size_t out_size) {
    struct tm tm_now;
    char filename[96];

    if (!dir || !dir[0] || !out_path || out_size == 0)
        return -1;
    if (!localtime_r(&now, &tm_now))
        return -1;

    if (suffix > 0) {
        snprintf(filename, sizeof(filename), "varnish-%04d%02d%02d-%02d%02d%02d-%02d.png",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, suffix);
    } else {
        snprintf(filename, sizeof(filename), "varnish-%04d%02d%02d-%02d%02d%02d.png",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }

    return path_join(out_path, out_size, dir, filename);
}

int screenshot_resolve_output_path(const char *dir, time_t now,
                                   char *out_path, size_t out_size) {
    int suffix = 0;

    if (!dir || !dir[0] || !out_path || out_size == 0)
        return -1;

    while (suffix < 100) {
        if (screenshot_build_candidate_path(dir, now, suffix, out_path, out_size) != 0)
            return -1;
        if (access(out_path, F_OK) != 0)
            return 0;
        suffix++;
    }

    return -1;
}

#ifdef __linux__
static void screenshot_mkdirp(const char *path) {
    char tmp[SCREENSHOT_MAX_PATH];
    char *p;

    if (!path || !path[0]) return;
    str_copy_trunc(tmp, sizeof(tmp), path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static const char *screenshot_output_dir(void) {
    const char *dir = getenv("VARNISH_SCREENSHOT_DIR");
    if (dir && dir[0])
        return dir;
    return SCREENSHOT_DEFAULT_DIR;
}

static uint8_t screenshot_scale_channel(uint32_t value, unsigned length) {
    uint32_t max_value;

    if (length == 0)
        return 0;
    if (length >= 8)
        return (uint8_t)(value >> (length - 8));

    max_value = (1u << length) - 1u;
    return (uint8_t)((value * 255u + (max_value / 2u)) / max_value);
}

static int screenshot_read_pixel(const uint8_t *src, unsigned bits_per_pixel,
                                 const struct fb_bitfield *red,
                                 const struct fb_bitfield *green,
                                 const struct fb_bitfield *blue,
                                 const struct fb_bitfield *alpha,
                                 uint8_t *out_r, uint8_t *out_g,
                                 uint8_t *out_b, uint8_t *out_a) {
    uint32_t raw = 0u;

    switch (bits_per_pixel) {
        case 16:
            raw = (uint32_t)src[0] | ((uint32_t)src[1] << 8);
            break;
        case 24:
            raw = (uint32_t)src[0] |
                  ((uint32_t)src[1] << 8) |
                  ((uint32_t)src[2] << 16);
            break;
        case 32:
            raw = (uint32_t)src[0] |
                  ((uint32_t)src[1] << 8) |
                  ((uint32_t)src[2] << 16) |
                  ((uint32_t)src[3] << 24);
            break;
        default:
            return -1;
    }

    *out_r = screenshot_scale_channel((raw >> red->offset) & ((1u << red->length) - 1u),
                                      red->length);
    *out_g = screenshot_scale_channel((raw >> green->offset) & ((1u << green->length) - 1u),
                                      green->length);
    *out_b = screenshot_scale_channel((raw >> blue->offset) & ((1u << blue->length) - 1u),
                                      blue->length);
    *out_a = alpha && alpha->length
        ? screenshot_scale_channel((raw >> alpha->offset) & ((1u << alpha->length) - 1u),
                                   alpha->length)
        : 255u;
    return 0;
}

static int screenshot_capture_linux(char *out_path, size_t out_size) {
    const char *dir = screenshot_output_dir();
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    SDL_Surface *surface = NULL;
    uint8_t *row = NULL;
    int fd = -1;
    int rc = -1;
    int bytes_per_pixel;
    off_t base_offset;

    screenshot_mkdirp(dir);
    if (screenshot_resolve_output_path(dir, time(NULL), out_path, out_size) != 0)
        return -1;

    fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0)
        return -1;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
        goto cleanup;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0)
        goto cleanup;
    if (vinfo.bits_per_pixel != 16 &&
        vinfo.bits_per_pixel != 24 &&
        vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "varnish: screenshot: unsupported bpp=%u\n",
                vinfo.bits_per_pixel);
        goto cleanup;
    }

    bytes_per_pixel = (int)((vinfo.bits_per_pixel + 7u) / 8u);
    base_offset = (off_t)vinfo.yoffset * (off_t)finfo.line_length +
                  (off_t)vinfo.xoffset * (off_t)bytes_per_pixel;

    surface = SDL_CreateRGBSurfaceWithFormat(0, (int)vinfo.xres, (int)vinfo.yres,
                                             32, SDL_PIXELFORMAT_RGBA32);
    if (!surface)
        goto cleanup;

    row = (uint8_t *)malloc((size_t)vinfo.xres * (size_t)bytes_per_pixel);
    if (!row)
        goto cleanup;

    for (uint32_t y = 0; y < vinfo.yres; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)surface->pixels + y * surface->pitch);
        ssize_t got = pread(fd, row, (size_t)vinfo.xres * (size_t)bytes_per_pixel,
                            base_offset + (off_t)y * (off_t)finfo.line_length);
        if (got != (ssize_t)((size_t)vinfo.xres * (size_t)bytes_per_pixel))
            goto cleanup;

        for (uint32_t x = 0; x < vinfo.xres; x++) {
            uint8_t r, g, b, a;
            if (screenshot_read_pixel(row + x * (uint32_t)bytes_per_pixel,
                                      vinfo.bits_per_pixel,
                                      &vinfo.red, &vinfo.green, &vinfo.blue, &vinfo.transp,
                                      &r, &g, &b, &a) != 0) {
                goto cleanup;
            }
            dst[x] = SDL_MapRGBA(surface->format, r, g, b, a);
        }
    }

    if (IMG_SavePNG(surface, out_path) != 0) {
        fprintf(stderr, "varnish: screenshot: save failed: %s\n", IMG_GetError());
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (row) free(row);
    if (surface) SDL_FreeSurface(surface);
    if (fd >= 0) close(fd);
    if (rc != 0 && out_path && out_size > 0)
        out_path[0] = '\0';
    return rc;
}
#endif

int screenshot_capture(char *out_path, size_t out_size) {
#ifdef __linux__
    return screenshot_capture_linux(out_path, out_size);
#else
    (void)out_path;
    (void)out_size;
    return -1;
#endif
}
