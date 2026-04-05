/*
 * hooks.c — Boot hook and preload wrapper management for Varnish.
 *
 * Manages two hook types:
 *  1. Boot hook (boot.d/varnish.sh) — starts the daemon at boot
 *  2. Preload wrapper — replaces nextui.elf with a script that sets LD_PRELOAD
 */

#include "hooks.h"
#include "device.h"
#include "strutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PATH 512

/* ── Path helpers ──────────────────────────────────────────────── */

static void mkdirp(const char *path) {
    char tmp[MAX_PATH];
    str_copy_trunc(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void get_hook_dir(const char *category, char *out, size_t size) {
    char ud[MAX_PATH];
    device_get_userdata_path(ud, sizeof(ud));
    if (size > 0) {
        if (!ud[0] ||
            path_join(out, size, ud, ".hooks") != 0 ||
            path_join(out, size, out, category) != 0) {
            out[0] = '\0';
        }
    }
}

/* ── Script content ────────────────────────────────────────────── */

static const char *boot_script =
    "#!/bin/sh\n"
    "# Varnish: start overlay daemon at boot\n"
    "PAK_DIR=\"/mnt/SDCARD/Tools/${PLATFORM}/Varnish.pak\"\n"
    "[ -x \"$PAK_DIR/varnish\" ] || exit 0\n"
    "cd \"$PAK_DIR\"\n"
    "export LD_LIBRARY_PATH=\"/usr/trimui/lib:$PAK_DIR/lib:$LD_LIBRARY_PATH\"\n"
    "./varnish --daemon &\n";

static const char *preload_wrapper_script =
    "#!/bin/sh\n"
    "# Varnish: LD_PRELOAD wrapper for multi-slot overlay compositing\n"
    "PLATFORM=\"${PLATFORM:-tg5040}\"\n"
    "SO=\"/mnt/SDCARD/Tools/${PLATFORM}/Varnish.pak/varnish_overlay.so\"\n"
    "[ -f \"$SO\" ] && export LD_PRELOAD=\"$SO${LD_PRELOAD:+:$LD_PRELOAD}\"\n"
    "exec \"$(dirname \"$0\")/nextui.elf.real\" \"$@\"\n";

/* ── Write/remove script helpers ───────────────────────────────── */

static int write_script(const char *dir, const char *filename, const char *content) {
    char path[MAX_PATH];

    if (!dir[0]) return -1;
    mkdirp(dir);

    if (path_join(path, sizeof(path), dir, filename) != 0) return -1;

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "varnish: cannot write hook %s: %s\n", path, strerror(errno));
        return -1;
    }
    fputs(content, f);
    fclose(f);
    chmod(path, 0755);

    fprintf(stderr, "varnish: installed hook %s\n", path);
    return 0;
}

static int remove_script(const char *dir, const char *filename) {
    char path[MAX_PATH];
    if (!dir[0] || path_join(path, sizeof(path), dir, filename) != 0)
        return -1;
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "varnish: cannot remove hook %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int script_exists(const char *dir, const char *filename) {
    char path[MAX_PATH];
    if (!dir[0] || path_join(path, sizeof(path), dir, filename) != 0)
        return 0;
    return access(path, F_OK) == 0;
}

/* ── Boot hook ─────────────────────────────────────────────────── */

int hooks_install_boot(void) {
    char dir[MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return write_script(dir, "varnish.sh", boot_script);
}

int hooks_uninstall_boot(void) {
    char dir[MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return remove_script(dir, "varnish.sh");
}

bool hooks_boot_installed(void) {
    char dir[MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return script_exists(dir, "varnish.sh");
}

/* ── Preload wrapper ───────────────────────────────────────────── */

int hooks_install_preload(void) {
#ifdef PLATFORM_MAC
    return 0;
#else
    char bin_dir[MAX_PATH];
    char elf_path[MAX_PATH + 32];
    char real_path[MAX_PATH + 32];

    device_get_system_bin_path(bin_dir, sizeof(bin_dir));
    if (!bin_dir[0]) return -1;

    snprintf(elf_path, sizeof(elf_path), "%s/nextui.elf", bin_dir);
    snprintf(real_path, sizeof(real_path), "%s/nextui.elf.real", bin_dir);

    /* Already wrapped? */
    if (access(real_path, X_OK) == 0) {
        fprintf(stderr, "varnish: preload wrapper already installed\n");
        return 0;
    }

    /* Verify the original binary exists */
    if (access(elf_path, X_OK) != 0) {
        fprintf(stderr, "varnish: nextui.elf not found at %s\n", elf_path);
        return -1;
    }

    /* Check it's actually a binary (starts with ELF magic), not already a script */
    {
        FILE *f = fopen(elf_path, "rb");
        if (!f) return -1;
        unsigned char hdr[4] = {0};
        size_t n = fread(hdr, 1, 4, f);
        fclose(f);
        if (n < 4 || hdr[0] != 0x7F || hdr[1] != 'E' ||
            hdr[2] != 'L' || hdr[3] != 'F') {
            fprintf(stderr, "varnish: %s is not an ELF binary, skipping wrapper\n",
                    elf_path);
            return -1;
        }
    }

    /* Rename original binary */
    if (rename(elf_path, real_path) != 0) {
        fprintf(stderr, "varnish: cannot rename %s -> %s: %s\n",
                elf_path, real_path, strerror(errno));
        return -1;
    }

    /* Write wrapper script */
    FILE *f = fopen(elf_path, "w");
    if (!f) {
        /* Roll back */
        rename(real_path, elf_path);
        fprintf(stderr, "varnish: cannot write wrapper: %s\n", strerror(errno));
        return -1;
    }
    fputs(preload_wrapper_script, f);
    fclose(f);
    chmod(elf_path, 0755);

    fprintf(stderr, "varnish: preload wrapper installed at %s\n", elf_path);
    return 0;
#endif
}

int hooks_uninstall_preload(void) {
#ifdef PLATFORM_MAC
    return 0;
#else
    char bin_dir[MAX_PATH];
    char elf_path[MAX_PATH + 32];
    char real_path[MAX_PATH + 32];

    device_get_system_bin_path(bin_dir, sizeof(bin_dir));
    if (!bin_dir[0]) return -1;

    snprintf(elf_path, sizeof(elf_path), "%s/nextui.elf", bin_dir);
    snprintf(real_path, sizeof(real_path), "%s/nextui.elf.real", bin_dir);

    if (access(real_path, X_OK) != 0) return 0; /* Not wrapped */

    /* Remove wrapper script, restore original */
    unlink(elf_path);
    if (rename(real_path, elf_path) != 0) {
        fprintf(stderr, "varnish: cannot restore %s: %s\n", elf_path, strerror(errno));
        return -1;
    }

    fprintf(stderr, "varnish: preload wrapper removed\n");
    return 0;
#endif
}

bool hooks_preload_installed(void) {
#ifdef PLATFORM_MAC
    return false;
#else
    char bin_dir[MAX_PATH];
    char real_path[MAX_PATH + 32];

    device_get_system_bin_path(bin_dir, sizeof(bin_dir));
    if (!bin_dir[0]) return false;

    snprintf(real_path, sizeof(real_path), "%s/nextui.elf.real", bin_dir);
    return access(real_path, X_OK) == 0;
#endif
}
