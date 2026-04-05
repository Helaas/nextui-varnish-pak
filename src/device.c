/*
 * device.c — Platform detection, path resolution, and device metrics.
 */

#include "device.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#endif

#define MAX_PATH 512

/* ── Path helpers ──────────────────────────────────────────────── */

void device_get_userdata_path(char *out, size_t size) {
    const char *p = getenv("USERDATA_PATH");
    if (p) {
        str_copy_trunc(out, size, p);
        return;
    }
#ifndef PLATFORM_MAC
    const char *sd = getenv("SDCARD_PATH");
    if (!sd) sd = "/mnt/SDCARD";
    const char *platform = getenv("PLATFORM");
    if (!platform) platform = "tg5040";
    if (size > 0) {
        if (path_join(out, size, sd, ".userdata") != 0 ||
            path_join(out, size, out, platform) != 0) {
            out[0] = '\0';
        }
    }
#else
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    if (size > 0) {
        if (path_join(out, size, home, ".userdata") != 0 ||
            path_join(out, size, out, "desktop") != 0) {
            out[0] = '\0';
        }
    }
#endif
}

void device_get_shared_userdata_path(char *out, size_t size) {
    const char *p = getenv("SHARED_USERDATA_PATH");
    if (p && p[0]) {
        str_copy_trunc(out, size, p);
        return;
    }
#ifndef PLATFORM_MAC
    const char *sd = getenv("SDCARD_PATH");
    if (!sd) sd = "/mnt/SDCARD";
    if (size > 0) {
        if (path_join(out, size, sd, ".userdata") != 0 ||
            path_join(out, size, out, "shared") != 0) {
            out[0] = '\0';
        }
    }
#else
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    if (size > 0) {
        if (path_join(out, size, home, ".userdata") != 0 ||
            path_join(out, size, out, "shared") != 0) {
            out[0] = '\0';
        }
    }
#endif
}

void device_get_system_bin_path(char *out, size_t size) {
    const char *sys = getenv("SYSTEM_PATH");
    if (sys && sys[0]) {
        snprintf(out, size, "%s/bin", sys);
        return;
    }
#ifndef PLATFORM_MAC
    const char *sd = getenv("SDCARD_PATH");
    if (!sd || !sd[0]) sd = "/mnt/SDCARD";
    const char *platform = getenv("PLATFORM");
    if (!platform || !platform[0]) {
#if defined(PLATFORM_TG5040)
        platform = "tg5040";
#elif defined(PLATFORM_TG5050)
        platform = "tg5050";
#elif defined(PLATFORM_MY355)
        platform = "my355";
#else
        platform = "tg5040";
#endif
    }
    snprintf(out, size, "%s/.system/%s/bin", sd, platform);
#else
    out[0] = '\0';
#endif
}

void device_get_pak_dir(char *out, size_t size) {
    const char *pak_dir = getenv("PAK_DIR");
    if (pak_dir && pak_dir[0]) {
        str_copy_trunc(out, size, pak_dir);
        return;
    }

#ifndef PLATFORM_MAC
    const char *sd = getenv("SDCARD_PATH");
    if (!sd || !sd[0]) sd = "/mnt/SDCARD";
    const char *platform = getenv("PLATFORM");
    if (!platform || !platform[0]) {
#if defined(PLATFORM_TG5040)
        platform = "tg5040";
#elif defined(PLATFORM_TG5050)
        platform = "tg5050";
#elif defined(PLATFORM_MY355)
        platform = "my355";
#else
        platform = "tg5040";
#endif
    }
    snprintf(out, size, "%s/Tools/%s/Varnish.pak", sd, platform);
#else
    const char *sd = getenv("SDCARD_PATH");
    const char *platform = getenv("PLATFORM");

    if (!sd || !sd[0] || !platform || !platform[0]) {
        out[0] = '\0';
        return;
    }

    snprintf(out, size, "%s/Tools/%s/Varnish.pak", sd, platform);
#endif
}

int device_get_fb_dimensions(int *out_w, int *out_h) {
#ifdef __linux__
    struct fb_var_screeninfo vinfo;
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) {
        perror("varnish: open /dev/fb0");
        return -1;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("varnish: ioctl FBIOGET_VSCREENINFO");
        close(fd);
        return -1;
    }
    close(fd);

    if (vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "varnish: fb bpp=%d, expected 32\n", vinfo.bits_per_pixel);
        return -1;
    }

    if (out_w) *out_w = (int)vinfo.xres;
    if (out_h) *out_h = (int)vinfo.yres;
    return 0;
#else
    /* macOS fallback for development */
    if (out_w) *out_w = 1024;
    if (out_h) *out_h = 768;
    return 0;
#endif
}

int device_get_scale(int fb_width, int fb_height) {
    if (fb_width == 1024 && fb_height == 768) {
        /* TrimUI Brick */
        return 3;
    }
    /* Smart Pro 1280x720, Miyoo Flip 640x480 */
    return 2;
}

int device_get_padding(int fb_width, int fb_height) {
    if (fb_width == 1024 && fb_height == 768) {
        return 5;
    }
    return 10;
}
