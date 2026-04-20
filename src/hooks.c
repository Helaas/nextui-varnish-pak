/*
 * hooks.c — Enabled marker, startup script patching, and boot hook management.
 */

#include "hooks.h"

#include "daemon.h"
#include "device.h"
#include "ipc.h"
#include "strutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PATH 512
#define STARTUP_MARKER_START "# >>> VARNISH STARTUP >>>"
#define STARTUP_MARKER_END   "# <<< VARNISH STARTUP <<<"
#define STARTUP_ANCHOR       "LAUNCH_PATH=\"$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak/launch.sh\""

static const char *enabled_marker_name = "enabled";
static const char *boot_hook_name = "varnish.sync.sh";

/* ── Path helpers ──────────────────────────────────────────────── */

static void mkdirp(const char *path) {
    char tmp[MAX_PATH];
    char *p;

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

static const char *default_platform(void) {
#if defined(PLATFORM_TG5050)
    return "tg5050";
#elif defined(PLATFORM_MY355)
    return "my355";
#else
    return "tg5040";
#endif
}

static void get_platform_name(char *out, size_t size) {
    const char *platform = getenv("PLATFORM");
    if (!platform || !platform[0]) platform = default_platform();
    str_copy_trunc(out, size, platform);
}

static void get_sdcard_path(char *out, size_t size) {
    const char *sd = getenv("SDCARD_PATH");
    if (!sd || !sd[0]) sd = "/mnt/SDCARD";
    str_copy_trunc(out, size, sd);
}

static void get_state_dir(char *out, size_t size) {
    char ud[MAX_PATH];
    device_get_userdata_path(ud, sizeof(ud));
    if (size > 0) {
        if (!ud[0] || path_join(out, size, ud, "Varnish") != 0)
            out[0] = '\0';
    }
}

static void get_enabled_marker_path(char *out, size_t size) {
    char dir[MAX_PATH];
    get_state_dir(dir, sizeof(dir));
    if (size > 0) {
        if (!dir[0] || path_join(out, size, dir, enabled_marker_name) != 0)
            out[0] = '\0';
    }
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

static void get_boot_hook_path(char *out, size_t size) {
    char dir[MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    if (size > 0) {
        if (!dir[0] || path_join(out, size, dir, boot_hook_name) != 0)
            out[0] = '\0';
    }
}

static void get_startup_script_path(char *out, size_t size) {
    char sd[MAX_PATH];
    char platform[32];
    char tmp_update[MAX_PATH];
    char script_name[64];

    get_sdcard_path(sd, sizeof(sd));
    get_platform_name(platform, sizeof(platform));

    if (size == 0) return;
    if (!sd[0] ||
        path_join(tmp_update, sizeof(tmp_update), sd, ".tmp_update") != 0 ||
        snprintf(script_name, sizeof(script_name), "%s.sh", platform) >= (int)sizeof(script_name) ||
        path_join(out, size, tmp_update, script_name) != 0) {
        out[0] = '\0';
    }
}

static void get_overlay_path(char *out, size_t size) {
    char pak_dir[MAX_PATH];
    device_get_pak_dir(pak_dir, sizeof(pak_dir));
    if (size > 0) {
        if (!pak_dir[0] || path_join(out, size, pak_dir, "varnish_overlay.so") != 0)
            out[0] = '\0';
    }
}

static int shell_quote_single(const char *src, char *dst, size_t dst_size) {
    size_t pos = 0;

    if (!src || !dst || dst_size < 3)
        return -1;

    dst[pos++] = '\'';
    while (*src) {
        unsigned char ch = (unsigned char)*src++;

        if (ch < 0x20 || ch == 0x7f)
            return -1;

        if (ch == '\'') {
            if (pos + 4 >= dst_size)
                return -1;
            dst[pos++] = '\'';
            dst[pos++] = '\\';
            dst[pos++] = '\'';
            dst[pos++] = '\'';
            continue;
        }

        if (pos + 2 > dst_size)
            return -1;
        dst[pos++] = (char)ch;
    }

    if (pos + 2 > dst_size)
        return -1;
    dst[pos++] = '\'';
    dst[pos] = '\0';
    return 0;
}

/* ── Script content ────────────────────────────────────────────── */

static const char *boot_script =
    "#!/bin/sh\n"
    "# Varnish: repair startup wiring and keep the daemon healthy\n"
    "PAK_DIR=\"/mnt/SDCARD/Tools/${PLATFORM}/Varnish.pak\"\n"
    "[ -x \"$PAK_DIR/varnish\" ] || exit 0\n"
    "cd \"$PAK_DIR\" || exit 0\n"
    "export LD_LIBRARY_PATH=\"$PAK_DIR/lib:${LD_LIBRARY_PATH:-}\"\n"
    "\"./varnish\" --boot-hook >/dev/null 2>&1\n"
    "RC=$?\n"
    "if [ \"$RC\" -eq 2 ]; then\n"
    "    sync\n"
    "    reboot\n"
    "fi\n"
    "exit 0\n";

static const char *startup_block =
    STARTUP_MARKER_START "\n"
    "VARNISH_PAK_DIR=\"$SDCARD_PATH/Tools/$PLATFORM/Varnish.pak\"\n"
    "if [ -x \"$VARNISH_PAK_DIR/varnish\" ]; then\n"
    "    VARNISH_ENV=$(\n"
    "        \"$VARNISH_PAK_DIR/varnish\" --startup-env 2>/dev/null\n"
    "    )\n"
    "    if [ -n \"$VARNISH_ENV\" ]; then\n"
    "        eval \"$VARNISH_ENV\"\n"
    "    fi\n"
    "fi\n"
    "unset VARNISH_ENV\n"
    "unset VARNISH_PAK_DIR\n"
    STARTUP_MARKER_END "\n";

/* ── File helpers ──────────────────────────────────────────────── */

static int read_text_file(const char *path, char **out_buf, size_t *out_len) {
    FILE *f;
    struct stat st;
    char *buf;
    size_t nread;

    if (!path || !path[0] || !out_buf) return -1;
    *out_buf = NULL;
    if (out_len) *out_len = 0;

    if (stat(path, &st) != 0) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;

    buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    nread = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (nread != (size_t)st.st_size) {
        free(buf);
        return -1;
    }

    buf[nread] = '\0';
    *out_buf = buf;
    if (out_len) *out_len = nread;
    return 0;
}

static int write_text_file(const char *path, const char *content, mode_t mode) {
    FILE *f;

    if (!path || !path[0] || !content) return -1;
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "varnish: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (fputs(content, f) == EOF) {
        fclose(f);
        return -1;
    }
    fclose(f);
    chmod(path, mode);
    return 0;
}

static int remove_path_if_exists(const char *path) {
    if (!path || !path[0]) return -1;
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "varnish: cannot remove %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int write_script(const char *dir, const char *filename, const char *content) {
    char path[MAX_PATH];

    if (!dir[0]) return -1;
    mkdirp(dir);

    if (path_join(path, sizeof(path), dir, filename) != 0) return -1;
    if (write_text_file(path, content, 0755) != 0) return -1;

    fprintf(stderr, "varnish: installed hook %s\n", path);
    return 0;
}

static int marker_block_bounds(const char *content, size_t *out_start, size_t *out_end) {
    const char *start;
    const char *end;

    if (!content) return -1;

    start = strstr(content, STARTUP_MARKER_START);
    if (!start) return -1;

    end = strstr(start, STARTUP_MARKER_END);
    if (!end) return -1;

    end += strlen(STARTUP_MARKER_END);
    if (*end == '\n') end++;

    if (out_start) *out_start = (size_t)(start - content);
    if (out_end) *out_end = (size_t)(end - content);
    return 0;
}

static int strip_startup_block(const char *content, char **out_buf) {
    size_t start = 0, end = 0;
    size_t len;
    char *buf;

    if (!content || !out_buf) return -1;
    *out_buf = NULL;

    if (marker_block_bounds(content, &start, &end) != 0)
        return 0;

    len = strlen(content);
    buf = (char *)malloc(len - (end - start) + 1);
    if (!buf) return -1;

    memcpy(buf, content, start);
    memcpy(buf + start, content + end, len - end + 1);
    *out_buf = buf;
    return 1;
}

static int insert_startup_block(const char *content, char **out_buf) {
    const char *anchor;
    size_t prefix_len;
    size_t block_len;
    size_t anchor_len;
    size_t len;
    char *buf;

    if (!content || !out_buf) return -1;
    *out_buf = NULL;

    if (strstr(content, STARTUP_MARKER_START) && strstr(content, STARTUP_MARKER_END))
        return 0;

    anchor = strstr(content, STARTUP_ANCHOR);
    if (!anchor) return -1;

    prefix_len = (size_t)(anchor - content);
    block_len = strlen(startup_block);
    anchor_len = strlen(content) - prefix_len;
    len = prefix_len + block_len + anchor_len;
    buf = (char *)malloc(len + 1);
    if (!buf) return -1;

    memcpy(buf, content, prefix_len);
    memcpy(buf + prefix_len, startup_block, block_len);
    memcpy(buf + prefix_len + block_len, anchor, anchor_len + 1);
    *out_buf = buf;
    return 1;
}

/* ── Enabled marker ────────────────────────────────────────────── */

int hooks_set_enabled(bool enabled) {
    char state_dir[MAX_PATH];
    char marker[MAX_PATH];

    get_state_dir(state_dir, sizeof(state_dir));
    get_enabled_marker_path(marker, sizeof(marker));
    if (!state_dir[0] || !marker[0]) return -1;

    if (!enabled)
        return remove_path_if_exists(marker);

    mkdirp(state_dir);
    return write_text_file(marker, "enabled\n", 0644);
}

bool hooks_is_enabled(void) {
    char marker[MAX_PATH];
    get_enabled_marker_path(marker, sizeof(marker));
    return marker[0] && access(marker, F_OK) == 0;
}

/* ── Startup patch ─────────────────────────────────────────────── */

int hooks_install_startup(void) {
    char startup_path[MAX_PATH];
    char *content = NULL;
    char *patched = NULL;
    struct stat st;
    int rc;
    mode_t mode = 0755;

    get_startup_script_path(startup_path, sizeof(startup_path));
    if (!startup_path[0]) return -1;
    if (read_text_file(startup_path, &content, NULL) != 0) {
        fprintf(stderr, "varnish: startup script not found at %s\n", startup_path);
        return -1;
    }

    rc = insert_startup_block(content, &patched);
    if (rc < 0) {
        free(content);
        fprintf(stderr, "varnish: could not locate startup patch anchor in %s\n",
                startup_path);
        return -1;
    }
    if (rc == 0) {
        free(content);
        return 0;
    }

    if (stat(startup_path, &st) == 0 && (st.st_mode & 0777))
        mode = st.st_mode & 0777;

    rc = write_text_file(startup_path, patched, mode);
    free(content);
    free(patched);
    return rc;
}

int hooks_uninstall_startup(void) {
    char startup_path[MAX_PATH];
    char *content = NULL;
    char *stripped = NULL;
    struct stat st;
    int rc;
    mode_t mode = 0755;

    get_startup_script_path(startup_path, sizeof(startup_path));
    if (!startup_path[0]) return -1;
    if (read_text_file(startup_path, &content, NULL) != 0)
        return 0;

    rc = strip_startup_block(content, &stripped);
    if (rc < 0) {
        free(content);
        return -1;
    }
    if (rc == 0) {
        free(content);
        return 0;
    }

    if (stat(startup_path, &st) == 0 && (st.st_mode & 0777))
        mode = st.st_mode & 0777;

    rc = write_text_file(startup_path, stripped, mode);
    free(content);
    free(stripped);
    return rc;
}

bool hooks_startup_installed(void) {
    char startup_path[MAX_PATH];
    char *content = NULL;
    bool installed = false;

    get_startup_script_path(startup_path, sizeof(startup_path));
    if (!startup_path[0]) return false;
    if (read_text_file(startup_path, &content, NULL) != 0)
        return false;

    installed = strstr(content, STARTUP_MARKER_START) != NULL &&
                strstr(content, STARTUP_MARKER_END) != NULL;
    free(content);
    return installed;
}

/* ── Boot hook ─────────────────────────────────────────────────── */

int hooks_install_boot(void) {
    char dir[MAX_PATH];
    get_hook_dir("boot.d", dir, sizeof(dir));
    return write_script(dir, boot_hook_name, boot_script);
}

int hooks_uninstall_boot(void) {
    char path[MAX_PATH];
    get_boot_hook_path(path, sizeof(path));
    if (!path[0]) return -1;
    return remove_path_if_exists(path);
}

bool hooks_boot_installed(void) {
    char path[MAX_PATH];
    get_boot_hook_path(path, sizeof(path));
    return path[0] && access(path, F_OK) == 0;
}

/* ── Runtime helpers ───────────────────────────────────────────── */

int hooks_write_startup_env(FILE *out) {
    char overlay_path[MAX_PATH];
    char overlay_path_quoted[MAX_PATH * 4 + 3];

    if (!out) return -1;
    if (!hooks_is_enabled()) return 0;

    get_overlay_path(overlay_path, sizeof(overlay_path));
    if (!overlay_path[0] || access(overlay_path, R_OK) != 0)
        return 0;
    if (shell_quote_single(overlay_path, overlay_path_quoted,
                           sizeof(overlay_path_quoted)) != 0)
        return -1;

    /* Re-apply the desired state after an in-app update replaced the script. */
    (void)hooks_install_startup();
    (void)hooks_install_boot();

    fprintf(out,
            "case \":${LD_PRELOAD:-}:\" in\n"
            "  *:%s:*) ;;\n"
            "  *) export LD_PRELOAD=%s${LD_PRELOAD:+\":$LD_PRELOAD\"} ;;\n"
            "esac\n",
            overlay_path_quoted, overlay_path_quoted);
    return ferror(out) ? -1 : 0;
}

int hooks_boot_check(const char *self_path) {
    if (!hooks_is_enabled())
        return 0;

    if (!hooks_startup_installed()) {
        if (hooks_install_startup() == 0)
            return 2;
        return 1;
    }

    if (!ipc_daemon_running() && daemon_spawn_background(self_path) < 0)
        return 1;

    return 0;
}
