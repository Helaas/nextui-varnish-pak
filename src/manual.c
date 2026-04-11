/*
 * manual.c — ScrapeGoat manual lookup and SDLReader handoff helpers.
 */

#include "manual.h"

#include "device.h"
#include "strutil.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static void manual_str_copy(char *dst, size_t dst_size, const char *src) {
    str_copy_trunc(dst, dst_size, src ? src : "");
}

static void manual_mkdirp(const char *path) {
    char tmp[VARNISH_MANUAL_PATH_MAX];
    char *p;

    if (!path || !path[0])
        return;

    manual_str_copy(tmp, sizeof(tmp), path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int manual_read_file(const char *path, char **out_buf, size_t *out_size) {
    FILE *f;
    long len;
    char *buf;

    if (!path || !out_buf)
        return -1;

    *out_buf = NULL;
    if (out_size)
        *out_size = 0;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    buf[len] = '\0';

    *out_buf = buf;
    if (out_size)
        *out_size = (size_t)len;
    return 0;
}

static int manual_write_file(const char *path, const char *data, size_t len) {
    FILE *f;

    if (!path || !data)
        return -1;

    f = fopen(path, "wb");
    if (!f)
        return -1;
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int manual_copy_file_if_exists(const char *src, const char *dst) {
    char *buf = NULL;
    size_t len = 0;
    int rc;

    if (manual_read_file(src, &buf, &len) != 0)
        return -1;

    rc = manual_write_file(dst, buf, len);
    free(buf);
    return rc;
}

static const char *manual_basename(const char *path) {
    const char *slash;

    if (!path)
        return "";
    slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

static void manual_dirname(const char *path, char *out, size_t out_size) {
    const char *slash;
    size_t len;

    if (!out || out_size == 0) return;
    out[0] = '\0';

    if (!path || !path[0])
        return;

    slash = strrchr(path, '/');
    if (!slash) {
        manual_str_copy(out, out_size, ".");
        return;
    }

    len = (size_t)(slash - path);
    if (len == 0) {
        manual_str_copy(out, out_size, "/");
        return;
    }
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static int manual_path_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

static int manual_extract_tag(const char *name, char *out, size_t out_size) {
    const char *open = NULL;
    const char *close = NULL;
    const char *start;
    const char *end;
    size_t len;

    if (!name || !name[0] || !out || out_size == 0)
        return -1;

    for (const char *p = name; *p; p++) {
        if (*p == '(')
            open = p;
        if (*p == ')')
            close = p;
    }
    if (!open || !close || close <= open)
        return -1;

    start = open + 1;
    while (start < close && isspace((unsigned char)*start))
        start++;
    end = close - 1;
    while (end > start && isspace((unsigned char)*end))
        end--;
    len = (size_t)(end - start + 1);
    if (len == 0 || len >= out_size)
        return -1;

    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static void manual_strip_extension(const char *name, char *out, size_t out_size) {
    const char *dot = strrchr(name, '.');
    size_t len;

    if (!out || out_size == 0)
        return;
    out[0] = '\0';

    if (!name || !name[0])
        return;

    if (dot && dot != name) {
        size_t extlen = strlen(dot);
        if (extlen >= 2 && extlen <= 5) {
            len = (size_t)(dot - name);
            if (len >= out_size)
                len = out_size - 1;
            memcpy(out, name, len);
            out[len] = '\0';
            return;
        }
    }

    manual_str_copy(out, out_size, name);
}

static int manual_json_extract_string(const char *json,
                                      const char *key,
                                      char *out,
                                      size_t out_size) {
    char needle[128];
    const char *hit;
    const char *colon;
    const char *start;
    const char *end;
    size_t len = 0;

    if (!json || !key || !out || out_size == 0)
        return -1;
    out[0] = '\0';

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    hit = strstr(json, needle);
    if (!hit)
        return -1;

    colon = strchr(hit + strlen(needle), ':');
    if (!colon)
        return -1;

    start = colon + 1;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (*start != '"')
        return -1;
    start++;

    end = start;
    while (*end) {
        if (*end == '"' && (end == start || end[-1] != '\\'))
            break;
        end++;
    }
    if (*end != '"')
        return -1;

    len = (size_t)(end - start);
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static size_t manual_json_escape(const char *src, char *dst, size_t dst_size) {
    size_t wrote = 0;

    if (!src || !dst || dst_size == 0)
        return 0;

    for (; *src && wrote + 1 < dst_size; src++) {
        const char *escape = NULL;
        char single[3] = {'\\', '\0', '\0'};

        switch (*src) {
            case '\\': single[1] = '\\'; escape = single; break;
            case '"':  single[1] = '"';  escape = single; break;
            case '\n': single[1] = 'n';  escape = single; break;
            case '\r': single[1] = 'r';  escape = single; break;
            case '\t': single[1] = 't';  escape = single; break;
            default: break;
        }

        if (escape) {
            if (wrote + 2 >= dst_size)
                break;
            dst[wrote++] = '\\';
            dst[wrote++] = escape[1];
        } else {
            dst[wrote++] = *src;
        }
    }

    dst[wrote] = '\0';
    return wrote;
}

static int manual_write_minimal_config(const char *path, const char *browse_dir) {
    char escaped[VARNISH_MANUAL_PATH_MAX * 2];
    char json[VARNISH_MANUAL_PATH_MAX * 2 + 64];

    manual_json_escape(browse_dir ? browse_dir : "", escaped, sizeof(escaped));
    snprintf(json, sizeof(json),
             "{\n"
             "  \"lastBrowseDirectory\": \"%s\"\n"
             "}\n",
             escaped);
    return manual_write_file(path, json, strlen(json));
}

static int manual_rewrite_last_browse_directory(const char *config_path,
                                                const char *browse_dir) {
    char *buf = NULL;
    size_t len = 0;
    char escaped[VARNISH_MANUAL_PATH_MAX * 2];
    char *hit;
    char *colon;
    char *start;
    char *end;
    char *rewritten;
    int rc;

    if (manual_read_file(config_path, &buf, &len) != 0)
        return manual_write_minimal_config(config_path, browse_dir);

    manual_json_escape(browse_dir ? browse_dir : "", escaped, sizeof(escaped));

    hit = strstr(buf, "\"lastBrowseDirectory\"");
    if (!hit) {
        char *closing = strrchr(buf, '}');
        size_t prefix_len;
        size_t needed;

        if (!closing) {
            free(buf);
            return manual_write_minimal_config(config_path, browse_dir);
        }

        prefix_len = (size_t)(closing - buf);
        needed = prefix_len + strlen(",\n  \"lastBrowseDirectory\": \"\"\n}\n") +
                 strlen(escaped) + 1;
        rewritten = (char *)malloc(needed);
        if (!rewritten) {
            free(buf);
            return -1;
        }

        memcpy(rewritten, buf, prefix_len);
        rewritten[prefix_len] = '\0';
        if (prefix_len > 0 && rewritten[prefix_len - 1] != '\n')
            str_append(rewritten, needed, "\n");
        str_append(rewritten, needed, ",\n  \"lastBrowseDirectory\": \"");
        str_append(rewritten, needed, escaped);
        str_append(rewritten, needed, "\"\n}\n");
        rc = manual_write_file(config_path, rewritten, strlen(rewritten));
        free(rewritten);
        free(buf);
        return rc;
    }

    colon = strchr(hit, ':');
    if (!colon) {
        free(buf);
        return manual_write_minimal_config(config_path, browse_dir);
    }
    start = colon + 1;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (*start != '"') {
        free(buf);
        return manual_write_minimal_config(config_path, browse_dir);
    }
    start++;
    end = start;
    while (*end) {
        if (*end == '"' && (end == start || end[-1] != '\\'))
            break;
        end++;
    }
    if (*end != '"') {
        free(buf);
        return manual_write_minimal_config(config_path, browse_dir);
    }

    rewritten = (char *)malloc(len + strlen(escaped) + 8);
    if (!rewritten) {
        free(buf);
        return -1;
    }

    memcpy(rewritten, buf, (size_t)(start - buf));
    rewritten[start - buf] = '\0';
    str_append(rewritten, len + strlen(escaped) + 8, escaped);
    str_append(rewritten, len + strlen(escaped) + 8, end);
    rc = manual_write_file(config_path, rewritten, strlen(rewritten));
    free(rewritten);
    free(buf);
    return rc;
}

static void manual_remove_file_if_exists(const char *path) {
    if (path && path[0])
        unlink(path);
}

static int manual_get_reader_paths(char *pak_dir, size_t pak_dir_size,
                                   char *reader_bin, size_t reader_bin_size,
                                   char *reader_state_dir, size_t reader_state_dir_size,
                                   char *log_path, size_t log_path_size) {
    char sdcard[VARNISH_MANUAL_PATH_MAX];
    char platform[32];
    char shared[VARNISH_MANUAL_PATH_MAX];
    char logs_dir[VARNISH_MANUAL_PATH_MAX];

    device_get_sdcard_path(sdcard, sizeof(sdcard));
    device_get_platform_name(platform, sizeof(platform));
    device_get_shared_userdata_path(shared, sizeof(shared));

    if (!sdcard[0] || !platform[0] || !shared[0])
        return -1;
    if (path_join(pak_dir, pak_dir_size, sdcard, "Tools") != 0 ||
        path_join(pak_dir, pak_dir_size, pak_dir, platform) != 0 ||
        path_join(pak_dir, pak_dir_size, pak_dir, "SDLReader.pak") != 0 ||
        path_join(reader_bin, reader_bin_size, pak_dir, "bin/sdl_reader_cli") != 0 ||
        path_join(reader_state_dir, reader_state_dir_size, shared, "SDLReader") != 0 ||
        path_join(logs_dir, sizeof(logs_dir), shared, "logs") != 0 ||
        path_join(log_path, log_path_size, logs_dir, "SDLReader.txt") != 0) {
        return -1;
    }

    return 0;
}

static int manual_resolve_rom_tag(const char *rom_path, char *out_tag, size_t out_tag_size) {
    char current[VARNISH_MANUAL_PATH_MAX];
    char parent[VARNISH_MANUAL_PATH_MAX];
    char sdcard[VARNISH_MANUAL_PATH_MAX];
    char roms_root[VARNISH_MANUAL_PATH_MAX];
    size_t root_len;

    if (!rom_path || !rom_path[0] || !out_tag || out_tag_size == 0)
        return -1;

    device_get_sdcard_path(sdcard, sizeof(sdcard));
    if (!sdcard[0] || path_join(roms_root, sizeof(roms_root), sdcard, "Roms") != 0)
        manual_str_copy(roms_root, sizeof(roms_root), "/mnt/SDCARD/Roms");

    manual_dirname(rom_path, current, sizeof(current));
    root_len = strlen(roms_root);

    while (current[0]) {
        const char *base = manual_basename(current);

        if (strncmp(current, roms_root, root_len) != 0 ||
            (current[root_len] != '\0' && current[root_len] != '/')) {
            break;
        }

        if (manual_extract_tag(base, out_tag, out_tag_size) == 0)
            return 0;

        if (strcmp(current, roms_root) == 0)
            break;
        manual_dirname(current, parent, sizeof(parent));
        if (strcmp(parent, current) == 0)
            break;
        manual_str_copy(current, sizeof(current), parent);
    }

    return -1;
}

void manual_lookup_init(varnish_manual_lookup *lookup) {
    if (!lookup)
        return;
    memset(lookup, 0, sizeof(*lookup));
}

int manual_read_download_dir(char *out, size_t out_size) {
    char shared[VARNISH_MANUAL_PATH_MAX];
    char settings_path[VARNISH_MANUAL_PATH_MAX];
    char fallback[VARNISH_MANUAL_PATH_MAX];
    char sdcard[VARNISH_MANUAL_PATH_MAX];
    char *json = NULL;

    if (!out || out_size == 0)
        return -1;
    out[0] = '\0';

    device_get_shared_userdata_path(shared, sizeof(shared));
    if (shared[0] && path_join(settings_path, sizeof(settings_path), shared,
                               "ScrapeGoat/settings.json") == 0 &&
        manual_read_file(settings_path, &json, NULL) == 0) {
        if (manual_json_extract_string(json, "manual_download_dir", out, out_size) == 0 &&
            out[0]) {
            free(json);
            return 0;
        }
        free(json);
    }

    device_get_sdcard_path(sdcard, sizeof(sdcard));
    if (!sdcard[0])
        manual_str_copy(sdcard, sizeof(sdcard), "/mnt/SDCARD");
    if (path_join(fallback, sizeof(fallback), sdcard, "Reader/Manuals") != 0)
        return -1;
    manual_str_copy(out, out_size, fallback);
    return 0;
}

int manual_parse_minarch_cmdline(const char *cmdline, size_t cmdline_len,
                                 char *out_rom_path, size_t out_rom_size) {
    size_t index = 0;
    int argi = 0;
    const char *candidate = NULL;

    if (!cmdline || cmdline_len == 0 || !out_rom_path || out_rom_size == 0)
        return -1;
    out_rom_path[0] = '\0';

    while (index < cmdline_len) {
        const char *arg = cmdline + index;
        size_t arg_len = strnlen(arg, cmdline_len - index);

        if (arg_len == 0) {
            index++;
            continue;
        }

        if (argi == 0) {
            if (!strstr(arg, "minarch.elf"))
                return -1;
        } else if (arg[0] == '/') {
            size_t n = strlen(arg);
            if (n < 3 || strcmp(arg + n - 3, ".so") != 0)
                candidate = arg;
        } else if (arg[0] != '-') {
            candidate = arg;
        }

        index += arg_len + 1;
        argi++;
    }

    if (!candidate || !candidate[0])
        return -1;

    manual_str_copy(out_rom_path, out_rom_size, candidate);
    return 0;
}

int manual_find_active_minarch(pid_t *out_pid,
                               char *out_rom_path, size_t out_rom_size) {
#ifdef __linux__
    DIR *dir;
    struct dirent *entry;
    pid_t best_pid = -1;
    char best_rom[VARNISH_MANUAL_PATH_MAX] = {0};

    if (out_pid)
        *out_pid = -1;
    if (out_rom_path && out_rom_size > 0)
        out_rom_path[0] = '\0';

    dir = opendir("/proc");
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        char cmdline_path[VARNISH_MANUAL_PATH_MAX];
        char buf[4096];
        int fd;
        ssize_t n;
        pid_t pid = 0;
        char rom_path[VARNISH_MANUAL_PATH_MAX];

        for (const char *p = entry->d_name; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                pid = 0;
                break;
            }
            pid = pid * 10 + (*p - '0');
        }
        if (pid <= 0)
            continue;

        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%s/cmdline", entry->d_name);
        fd = open(cmdline_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n <= 0)
            continue;

        if (manual_parse_minarch_cmdline(buf, (size_t)n, rom_path, sizeof(rom_path)) != 0)
            continue;

        if (pid > best_pid) {
            best_pid = pid;
            manual_str_copy(best_rom, sizeof(best_rom), rom_path);
        }
    }

    closedir(dir);

    if (best_pid <= 0)
        return -1;

    if (out_pid)
        *out_pid = best_pid;
    if (out_rom_path && out_rom_size > 0)
        manual_str_copy(out_rom_path, out_rom_size, best_rom);
    return 0;
#else
    (void)out_pid;
    (void)out_rom_path;
    (void)out_rom_size;
    return -1;
#endif
}

int manual_build_lookup_from_rom(const char *rom_path,
                                 const char *manual_root,
                                 varnish_manual_lookup *out) {
    char rom_name[VARNISH_MANUAL_NAME_MAX];
    char system_dir[VARNISH_MANUAL_PATH_MAX];

    if (!rom_path || !rom_path[0] || !manual_root || !manual_root[0] || !out)
        return -1;

    manual_lookup_init(out);
    manual_str_copy(out->manual_root, sizeof(out->manual_root), manual_root);

    if (manual_resolve_rom_tag(rom_path, out->system_tag, sizeof(out->system_tag)) != 0)
        return -1;

    manual_strip_extension(manual_basename(rom_path), rom_name, sizeof(rom_name));
    if (!rom_name[0])
        return -1;
    manual_str_copy(out->display_name, sizeof(out->display_name), rom_name);

    if (path_join(system_dir, sizeof(system_dir), manual_root, out->system_tag) != 0) {
        return -1;
    }

    manual_str_copy(out->manual_path, sizeof(out->manual_path), system_dir);
    if (str_append(out->manual_path, sizeof(out->manual_path), "/") != 0 ||
        str_append(out->manual_path, sizeof(out->manual_path), out->display_name) != 0 ||
        str_append(out->manual_path, sizeof(out->manual_path), ".pdf") != 0) {
        return -1;
    }

    if (manual_path_exists(system_dir))
        manual_str_copy(out->browse_dir, sizeof(out->browse_dir), system_dir);
    else
        manual_str_copy(out->browse_dir, sizeof(out->browse_dir), manual_root);

    out->exact_match = manual_path_exists(out->manual_path);
    return 0;
}

int manual_prepare_browser_state(const char *source_state_dir,
                                 const char *browse_dir,
                                 char *out_state_dir, size_t out_state_dir_size) {
    char template[] = "/tmp/varnish-manual-XXXXXX";
    char config_src[VARNISH_MANUAL_PATH_MAX];
    char config_dst[VARNISH_MANUAL_PATH_MAX];
    char history_src[VARNISH_MANUAL_PATH_MAX];
    char history_dst[VARNISH_MANUAL_PATH_MAX];
    char *dir;

    if (!browse_dir || !browse_dir[0] || !out_state_dir || out_state_dir_size == 0)
        return -1;

    dir = mkdtemp(template);
    if (!dir)
        return -1;
    manual_str_copy(out_state_dir, out_state_dir_size, dir);

    if (source_state_dir && source_state_dir[0]) {
        if (path_join(config_src, sizeof(config_src), source_state_dir, "config.json") == 0 &&
            path_join(history_src, sizeof(history_src), source_state_dir, "reading_history.json") == 0 &&
            path_join(config_dst, sizeof(config_dst), dir, "config.json") == 0 &&
            path_join(history_dst, sizeof(history_dst), dir, "reading_history.json") == 0) {
            (void)manual_copy_file_if_exists(config_src, config_dst);
            (void)manual_copy_file_if_exists(history_src, history_dst);
        }
    }

    if (path_join(config_dst, sizeof(config_dst), dir, "config.json") != 0)
        return -1;
    return manual_rewrite_last_browse_directory(config_dst, browse_dir);
}

void manual_cleanup_temp_state(const char *state_dir) {
    char config_path[VARNISH_MANUAL_PATH_MAX];
    char history_path[VARNISH_MANUAL_PATH_MAX];

    if (!state_dir || !state_dir[0])
        return;

    if (path_join(config_path, sizeof(config_path), state_dir, "config.json") == 0)
        manual_remove_file_if_exists(config_path);
    if (path_join(history_path, sizeof(history_path), state_dir, "reading_history.json") == 0)
        manual_remove_file_if_exists(history_path);
    rmdir(state_dir);
}

void manual_session_init(varnish_manual_session *session) {
    if (!session)
        return;
    memset(session, 0, sizeof(*session));
    session->minarch_pid = -1;
    session->reader_pid = -1;
}

int manual_session_begin(varnish_manual_session *session,
                         pid_t minarch_pid,
                         pid_t reader_pid,
                         const char *temp_state_dir) {
    if (!session || minarch_pid <= 0 || reader_pid <= 0)
        return -1;

    manual_session_init(session);
    session->active = true;
    session->minarch_pid = minarch_pid;
    session->reader_pid = reader_pid;
    session->minarch_stopped = true;
    manual_str_copy(session->temp_state_dir, sizeof(session->temp_state_dir), temp_state_dir);
    return 0;
}

static void manual_resume_minarch(varnish_manual_session *session) {
    if (!session || !session->minarch_stopped || session->minarch_pid <= 0)
        return;

    if (kill(session->minarch_pid, SIGCONT) == 0 || errno == ESRCH)
        session->minarch_stopped = false;
}

static void manual_reset_session(varnish_manual_session *session) {
    char temp_dir[VARNISH_MANUAL_PATH_MAX];

    if (!session)
        return;

    manual_str_copy(temp_dir, sizeof(temp_dir), session->temp_state_dir);
    manual_session_init(session);
    manual_cleanup_temp_state(temp_dir);
}

int manual_session_poll(varnish_manual_session *session) {
    int status = 0;
    pid_t rc;

    if (!session || !session->active || session->reader_pid <= 0)
        return 0;

    rc = waitpid(session->reader_pid, &status, WNOHANG);
    if (rc == 0)
        return 0;

    if (rc < 0 && errno != ECHILD)
        return 0;

    manual_resume_minarch(session);
    manual_reset_session(session);
    return 1;
}

void manual_session_abort(varnish_manual_session *session) {
    if (!session || !session->active)
        return;

    if (session->reader_pid > 0) {
        int status = 0;
        kill(session->reader_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            pid_t rc = waitpid(session->reader_pid, &status, WNOHANG);
            if (rc == session->reader_pid || (rc < 0 && errno == ECHILD))
                break;
            usleep(25000);
        }
        if (waitpid(session->reader_pid, &status, WNOHANG) == 0) {
            kill(session->reader_pid, SIGKILL);
            waitpid(session->reader_pid, &status, 0);
        }
    }

    manual_resume_minarch(session);
    manual_reset_session(session);
}

static int manual_spawn_reader(const char *reader_bin,
                               const char *pak_dir,
                               const char *state_dir,
                               const char *browse_dir,
                               const char *log_path,
                               const char *manual_path,
                               pid_t *out_pid) {
    int errpipe[2] = {-1, -1};
    pid_t pid;

    if (!reader_bin || !reader_bin[0] || !pak_dir || !pak_dir[0] ||
        !state_dir || !state_dir[0] || !log_path || !log_path[0] ||
        !out_pid) {
        return -1;
    }

    if (pipe(errpipe) != 0)
        return -1;
    (void)fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);

    pid = fork();
    if (pid < 0) {
        close(errpipe[0]);
        close(errpipe[1]);
        return -1;
    }

    if (pid == 0) {
        int log_fd;
        char pak_lib[VARNISH_MANUAL_PATH_MAX];
        const char *old_ld;
        char new_ld[VARNISH_MANUAL_PATH_MAX * 2];
        char shared[VARNISH_MANUAL_PATH_MAX];
        char logs_dir[VARNISH_MANUAL_PATH_MAX];
        char home_dir[VARNISH_MANUAL_PATH_MAX];
        char *argv_browse[] = { (char *)reader_bin, "-b", NULL };
        char *argv_doc[] = { (char *)reader_bin, (char *)manual_path, NULL };
        int exec_errno = 0;

        close(errpipe[0]);

        device_get_shared_userdata_path(shared, sizeof(shared));
        if (shared[0] && path_join(logs_dir, sizeof(logs_dir), shared, "logs") == 0)
            manual_mkdirp(logs_dir);
        manual_mkdirp(state_dir);

        if (chdir(pak_dir) != 0)
            goto child_fail;

        manual_str_copy(home_dir, sizeof(home_dir), state_dir);
        setenv("HOME", home_dir, 1);
        setenv("SDL_READER_STATE_DIR", state_dir, 1);
        if (browse_dir && browse_dir[0])
            setenv("SDL_READER_DEFAULT_DIR", browse_dir, 1);

        if (path_join(pak_lib, sizeof(pak_lib), pak_dir, "lib") == 0 &&
            manual_path_exists(pak_lib)) {
            old_ld = getenv("LD_LIBRARY_PATH");
            if (old_ld && old_ld[0]) {
                snprintf(new_ld, sizeof(new_ld), "%s:%s", pak_lib, old_ld);
                setenv("LD_LIBRARY_PATH", new_ld, 1);
            } else {
                setenv("LD_LIBRARY_PATH", pak_lib, 1);
            }
        }

        log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            if (log_fd > STDERR_FILENO)
                close(log_fd);
        }

        unsetenv("LD_PRELOAD");
        execv(reader_bin, manual_path && manual_path[0] ? argv_doc : argv_browse);

child_fail:
        exec_errno = errno ? errno : EIO;
        (void)write(errpipe[1], &exec_errno, sizeof(exec_errno));
        _exit(127);
    }

    close(errpipe[1]);
    {
        int child_errno = 0;
        ssize_t n = read(errpipe[0], &child_errno, sizeof(child_errno));
        close(errpipe[0]);
        if (n > 0) {
            waitpid(pid, NULL, 0);
            errno = child_errno;
            return -1;
        }
    }

    *out_pid = pid;
    return 0;
}

static int manual_start_reader_for_lookup(varnish_manual_session *session,
                                          pid_t minarch_pid,
                                          const varnish_manual_lookup *lookup) {
    char pak_dir[VARNISH_MANUAL_PATH_MAX];
    char reader_bin[VARNISH_MANUAL_PATH_MAX];
    char reader_state_dir[VARNISH_MANUAL_PATH_MAX];
    char log_path[VARNISH_MANUAL_PATH_MAX];
    char temp_state_dir[VARNISH_MANUAL_PATH_MAX] = {0};
    const char *state_dir;
    const char *manual_path;
    pid_t reader_pid = -1;

    if (!session || !lookup)
        return -1;
    if (manual_get_reader_paths(pak_dir, sizeof(pak_dir),
                                reader_bin, sizeof(reader_bin),
                                reader_state_dir, sizeof(reader_state_dir),
                                log_path, sizeof(log_path)) != 0) {
        return -1;
    }
    if (access(reader_bin, X_OK) != 0)
        return -1;

    manual_mkdirp(reader_state_dir);

    if (!lookup->exact_match) {
        if (manual_prepare_browser_state(reader_state_dir, lookup->browse_dir,
                                         temp_state_dir, sizeof(temp_state_dir)) != 0) {
            return -1;
        }
        state_dir = temp_state_dir;
        manual_path = NULL;
    } else {
        state_dir = reader_state_dir;
        manual_path = lookup->manual_path;
    }

    if (kill(minarch_pid, SIGSTOP) != 0) {
        manual_cleanup_temp_state(temp_state_dir);
        return -1;
    }

    if (manual_spawn_reader(reader_bin, pak_dir, state_dir, lookup->browse_dir,
                            log_path, manual_path, &reader_pid) != 0) {
        kill(minarch_pid, SIGCONT);
        manual_cleanup_temp_state(temp_state_dir);
        return -1;
    }

    return manual_session_begin(session, minarch_pid, reader_pid, temp_state_dir);
}

int manual_start_for_active_game(varnish_manual_session *session,
                                 char *message, size_t message_size) {
    pid_t minarch_pid = -1;
    char rom_path[VARNISH_MANUAL_PATH_MAX];
    char manual_root[VARNISH_MANUAL_PATH_MAX];
    char pak_dir[VARNISH_MANUAL_PATH_MAX];
    char reader_bin[VARNISH_MANUAL_PATH_MAX];
    char reader_state_dir[VARNISH_MANUAL_PATH_MAX];
    char log_path[VARNISH_MANUAL_PATH_MAX];
    varnish_manual_lookup lookup;

    if (message && message_size > 0)
        message[0] = '\0';

    if (!session)
        return -1;
    if (session->active) {
        manual_str_copy(message, message_size, "Manual already open");
        return -1;
    }

    if (manual_find_active_minarch(&minarch_pid, rom_path, sizeof(rom_path)) != 0) {
        manual_str_copy(message, message_size, "No active game");
        return -1;
    }

    if (manual_read_download_dir(manual_root, sizeof(manual_root)) != 0 ||
        manual_build_lookup_from_rom(rom_path, manual_root, &lookup) != 0) {
        manual_str_copy(message, message_size, "Could not resolve manual");
        return -1;
    }

    if (manual_get_reader_paths(pak_dir, sizeof(pak_dir),
                                reader_bin, sizeof(reader_bin),
                                reader_state_dir, sizeof(reader_state_dir),
                                log_path, sizeof(log_path)) != 0 ||
        access(reader_bin, X_OK) != 0) {
        manual_str_copy(message, message_size, "SDLReader not available");
        return -1;
    }

    if (manual_start_reader_for_lookup(session, minarch_pid, &lookup) != 0) {
        manual_str_copy(message, message_size, "Could not open manual");
        return -1;
    }

    return 0;
}
