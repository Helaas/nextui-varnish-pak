/*
 * recording.c — Daemon-side video recording session management.
 */

#include "recording.h"

#include "device.h"
#include "recording_profile.h"
#include "recording_shm.h"
#include "recording_transport.h"
#include "strutil.h"

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

#define RECORDING_MAX_PATH 512
#define RECORDING_ENCODER_PREFLIGHT_MS 500u
#define RECORDING_ENCODER_CLOSE_TIMEOUT_MS 2000u
#define RECORDING_ENCODER_TERM_TIMEOUT_MS 500u
#define RECORDING_ENCODER_WAIT_SLICE_US 20000u

static uint32_t recording_frame_pixels[VARNISH_RECORDING_MAX_W * VARNISH_RECORDING_MAX_H];

static void recording_mkdirp(const char *path) {
    char tmp[RECORDING_MAX_PATH];
    char *p;

    if (!path || !path[0])
        return;

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

static int recording_build_output_dir(char *out, size_t out_size) {
    const char *env_dir = getenv("VARNISH_VIDEO_DIR");
    char sdcard[RECORDING_MAX_PATH];

    if (!out || out_size == 0)
        return -1;

    if (env_dir && env_dir[0]) {
        str_copy_trunc(out, out_size, env_dir);
        return 0;
    }

    device_get_sdcard_path(sdcard, sizeof(sdcard));
    if (!sdcard[0])
        return -1;
    if (path_join(out, out_size, sdcard, "Videos") != 0 ||
        path_join(out, out_size, out, "Varnish") != 0) {
        return -1;
    }

    return 0;
}

static int recording_resolve_output_path(char *out_path, size_t out_size) {
    char dir[RECORDING_MAX_PATH];
    struct tm tm_now;
    time_t now;
    int suffix = 0;

    if (!out_path || out_size == 0)
        return -1;
    if (recording_build_output_dir(dir, sizeof(dir)) != 0)
        return -1;

    recording_mkdirp(dir);
    now = time(NULL);
    if (!localtime_r(&now, &tm_now))
        return -1;

    while (suffix < 100) {
        char filename[96];

        if (suffix > 0) {
            snprintf(filename, sizeof(filename),
                     "varnish-%04d%02d%02d-%02d%02d%02d-%02d.avi",
                     tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, suffix);
        } else {
            snprintf(filename, sizeof(filename),
                     "varnish-%04d%02d%02d-%02d%02d%02d.avi",
                     tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        }

        if (path_join(out_path, out_size, dir, filename) != 0)
            return -1;
        if (access(out_path, F_OK) != 0)
            return 0;
        suffix++;
    }

    return -1;
}

static int recording_resolve_ffmpeg_path(char *out_path, size_t out_size) {
    char pak_dir[RECORDING_MAX_PATH];
    char bin_dir[RECORDING_MAX_PATH];

    if (!out_path || out_size == 0)
        return -1;

    device_get_pak_dir(pak_dir, sizeof(pak_dir));
    if (pak_dir[0] &&
        path_join(bin_dir, sizeof(bin_dir), pak_dir, "bin") == 0 &&
        path_join(out_path, out_size, bin_dir, "ffmpeg") == 0 &&
        access(out_path, X_OK) == 0) {
        return 0;
    }

    str_copy_trunc(out_path, out_size, "ffmpeg");
    return 0;
}

static int recording_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0) {
        ssize_t wrote = write(fd, p, len);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (wrote == 0)
            return -1;
        p += (size_t)wrote;
        len -= (size_t)wrote;
    }

    return 0;
}

static uint64_t recording_monotonic_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int recording_wait_for_pid(pid_t pid, unsigned timeout_ms, int *out_status) {
    uint64_t start_ms = recording_monotonic_ms();

    for (;;) {
        int status = 0;
        pid_t rc = waitpid(pid, &status, WNOHANG);

        if (rc == pid) {
            if (out_status)
                *out_status = status;
            return 1;
        }
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (timeout_ms == 0u)
            return 0;
        if (recording_monotonic_ms() - start_ms >= (uint64_t)timeout_ms)
            return 0;
        usleep(RECORDING_ENCODER_WAIT_SLICE_US);
    }
}

static void recording_remove_output_file(const varnish_recording_session *session) {
    if (!session || !session->output_path[0])
        return;
    if (unlink(session->output_path) != 0 && errno != ENOENT)
        perror("varnish: recording unlink");
}

static void recording_capture_drop_count(varnish_recording_session *session,
                                         const varnish_recording_shm_t *shm) {
    if (!session || !shm)
        return;
    session->drop_count = shm->drop_count;
}

static void recording_log_summary(const varnish_recording_session *session,
                                  const char *result) {
    if (!session)
        return;
    fprintf(stderr,
            "varnish: recording %s file=%s source=%dx%d output=%dx%d frames=%u drops=%u exit=%d timeout=%d\n",
            result ? result : "finished",
            session->output_path[0] ? session->output_path : "(none)",
            session->source_w, session->source_h,
            session->output_w, session->output_h,
            session->frames_written, session->drop_count,
            session->encoder_exit_status, session->encoder_timed_out);
}

static int recording_finalize_encoder(varnish_recording_session *session,
                                      int graceful_stop) {
    int status = 0;
    int waited = 0;
    int success = 0;

    if (!session)
        return -1;

    if (session->encoder_pid > 0) {
        pid_t pid = (pid_t)session->encoder_pid;

        session->encoder_timed_out = 0;
        if (session->encoder_fd >= 0) {
            close(session->encoder_fd);
            session->encoder_fd = -1;
        }

        waited = recording_wait_for_pid(pid,
                                        graceful_stop ? RECORDING_ENCODER_CLOSE_TIMEOUT_MS : 0u,
                                        &status);
        if (waited == 0) {
            session->encoder_timed_out = 1;
            kill(pid, SIGTERM);
            waited = recording_wait_for_pid(pid, RECORDING_ENCODER_TERM_TIMEOUT_MS, &status);
        }
        if (waited == 0) {
            kill(pid, SIGKILL);
            if (waitpid(pid, &status, 0) == pid)
                waited = 1;
            else
                waited = -1;
        }

        session->encoder_pid = 0;
        session->encoder_exit_status = waited > 0
            ? recording_normalize_wait_status(status)
            : -1;
        success = (waited > 0 &&
                   !session->encoder_timed_out &&
                   session->encoder_exit_status == 0);
    } else if (session->encoder_fd >= 0) {
        close(session->encoder_fd);
        session->encoder_fd = -1;
    }

    if (!success)
        recording_remove_output_file(session);
    return success ? 0 : -1;
}

static int recording_preflight_encoder(varnish_recording_session *session) {
    int status = 0;
    int waited;

    if (!session || session->encoder_pid <= 0)
        return -1;

    waited = recording_wait_for_pid((pid_t)session->encoder_pid,
                                    RECORDING_ENCODER_PREFLIGHT_MS,
                                    &status);
    if (waited == 0)
        return 0;

    if (session->encoder_fd >= 0) {
        close(session->encoder_fd);
        session->encoder_fd = -1;
    }
    session->encoder_exit_status = (waited > 0)
        ? recording_normalize_wait_status(status)
        : -1;
    session->encoder_pid = 0;
    recording_remove_output_file(session);
    return -1;
}

static int recording_encoder_alive(varnish_recording_session *session) {
    int status = 0;
    pid_t rc;

    if (!session || session->encoder_pid <= 0)
        return 0;

    rc = waitpid((pid_t)session->encoder_pid, &status, WNOHANG);
    if (rc == 0)
        return 1;
    if (rc < 0) {
        if (errno == EINTR)
            return 1;
        session->encoder_exit_status = -1;
    } else {
        session->encoder_exit_status = recording_normalize_wait_status(status);
    }
    session->encoder_pid = 0;
    if (session->encoder_fd >= 0) {
        close(session->encoder_fd);
        session->encoder_fd = -1;
    }
    session->encoder_timed_out = 0;
    recording_remove_output_file(session);
    return 0;
}

static int recording_spawn_encoder(varnish_recording_session *session) {
    varnish_recording_encoder_profile profile;
    const char *argv_const[VARNISH_RECORDING_FFMPEG_ARGV_MAX];
    char *argv_exec[VARNISH_RECORDING_FFMPEG_ARGV_MAX];
    size_t argc;
    int pipe_fds[2] = { -1, -1 };
    pid_t pid;
    size_t i;

    if (recording_build_encoder_profile(&profile,
                                        session->source_w, session->source_h,
                                        session->cadence_ms,
                                        VARNISH_RECORDING_DEFAULT_OUT_W,
                                        VARNISH_RECORDING_DEFAULT_OUT_H) != 0) {
        return -1;
    }

    session->output_w = profile.output_w;
    session->output_h = profile.output_h;
    argc = recording_build_ffmpeg_argv(&profile, session->ffmpeg_path,
                                       session->output_path,
                                       argv_const,
                                       sizeof(argv_const) / sizeof(argv_const[0]));
    if (argc == 0u)
        return -1;
    for (i = 0u; i < argc; i++)
        argv_exec[i] = (char *)argv_const[i];
    argv_exec[argc] = NULL;

    if (pipe(pipe_fds) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);

        dup2(pipe_fds[0], STDIN_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(session->ffmpeg_path, argv_exec);
        _exit(127);
    }

    close(pipe_fds[0]);
    session->encoder_fd = pipe_fds[1];
    session->encoder_pid = (int)pid;
    return recording_preflight_encoder(session);
}

static int recording_drain_frames(varnish_recording_session *session) {
    varnish_recording_shm_t *shm = recording_shm_get();

    if (!session || !shm || session->encoder_fd < 0)
        return -1;

    for (;;) {
        varnish_recording_frame_info info;
        int rc = varnish_recording_pop_frame(
            shm, recording_frame_pixels,
            sizeof(recording_frame_pixels) / sizeof(recording_frame_pixels[0]),
            &info);
        size_t pixel_count;

        if (rc == 0)
            return 0;
        if (rc < 0)
            continue;

        pixel_count = (size_t)info.width * (size_t)info.height;
        if (recording_write_all(session->encoder_fd, recording_frame_pixels,
                                pixel_count * sizeof(uint32_t)) != 0) {
            return -1;
        }
        session->frames_written++;
    }
}

void recording_session_init(varnish_recording_session *session) {
    if (!session)
        return;

    memset(session, 0, sizeof(*session));
    session->encoder_fd = -1;
    session->encoder_exit_status = -1;
    session->output_w = VARNISH_RECORDING_DEFAULT_OUT_W;
    session->output_h = VARNISH_RECORDING_DEFAULT_OUT_H;
    session->cadence_ms = VARNISH_RECORDING_DEFAULT_CADENCE_MS;
}

int recording_init_transport(int source_w, int source_h) {
    return recording_shm_init(source_w, source_h);
}

void recording_shutdown_transport(void) {
    recording_shm_cleanup();
}

bool recording_session_active(const varnish_recording_session *session) {
    return session && session->active;
}

int recording_start(varnish_recording_session *session,
                    char *message, size_t message_size) {
    varnish_recording_shm_t *shm = recording_shm_get();

    if (message && message_size > 0)
        message[0] = '\0';
    if (!session || !shm) {
        str_copy_trunc(message, message_size, "Recorder unavailable");
        return -1;
    }
    if (session->active) {
        str_copy_trunc(message, message_size, "Already recording");
        return -1;
    }

    recording_session_init(session);
    session->initialized = 1;
    session->source_w = shm->source_w;
    session->source_h = shm->source_h;
    if (recording_resolve_output_path(session->output_path,
                                      sizeof(session->output_path)) != 0) {
        str_copy_trunc(message, message_size, "Could not create output path");
        return -1;
    }
    if (recording_resolve_ffmpeg_path(session->ffmpeg_path,
                                      sizeof(session->ffmpeg_path)) != 0) {
        str_copy_trunc(message, message_size, "Could not locate ffmpeg");
        return -1;
    }
    if (recording_spawn_encoder(session) != 0) {
        str_copy_trunc(message, message_size, "Could not start ffmpeg");
        recording_finalize_encoder(session, 0);
        return -1;
    }

    varnish_recording_set_active(shm, 1, session->cadence_ms,
                                 session->output_w, session->output_h);
    session->active = 1;
    return 0;
}

int recording_stop(varnish_recording_session *session,
                   char *message, size_t message_size) {
    const char *filename;
    varnish_recording_shm_t *shm = recording_shm_get();
    int success;

    if (message && message_size > 0)
        message[0] = '\0';
    if (!session || !session->active || !shm) {
        str_copy_trunc(message, message_size, "Recorder not active");
        return -1;
    }

    varnish_recording_set_active(shm, 0, 0u, 0, 0);
    recording_capture_drop_count(session, shm);
    if (recording_drain_frames(session) != 0) {
        recording_finalize_encoder(session, 0);
        session->active = 0;
        recording_log_summary(session, "failed");
        str_copy_trunc(message, message_size, "Recording failed");
        return -1;
    }

    success = recording_finalize_encoder(session, 1);
    session->active = 0;
    if (success != 0) {
        recording_log_summary(session, "failed");
        str_copy_trunc(message, message_size, "Recording failed");
        return -1;
    }

    filename = strrchr(session->output_path, '/');
    filename = filename ? (filename + 1) : session->output_path;
    recording_log_summary(session, "saved");
    str_copy_trunc(message, message_size, "Saved ");
    (void)str_append(message, message_size, filename);
    return 0;
}

int recording_toggle(varnish_recording_session *session,
                     char *message, size_t message_size,
                     int *out_started) {
    if (out_started)
        *out_started = 0;
    if (recording_session_active(session)) {
        if (recording_stop(session, message, message_size) != 0)
            return -1;
        return 0;
    }

    if (recording_start(session, message, message_size) != 0)
        return -1;

    if (out_started)
        *out_started = 1;
    return 0;
}

int recording_poll(varnish_recording_session *session,
                   char *message, size_t message_size) {
    varnish_recording_shm_t *shm = recording_shm_get();

    if (message && message_size > 0)
        message[0] = '\0';
    if (!session || !session->active)
        return 0;
    if (!recording_encoder_alive(session)) {
        if (shm)
            recording_capture_drop_count(session, shm);
        session->active = 0;
        session->encoder_failures++;
        recording_log_summary(session, "failed");
        str_copy_trunc(message, message_size, "Recording failed");
        return -1;
    }

    if (recording_drain_frames(session) != 0) {
        if (shm)
            recording_capture_drop_count(session, shm);
        recording_finalize_encoder(session, 0);
        session->active = 0;
        session->encoder_failures++;
        recording_log_summary(session, "failed");
        str_copy_trunc(message, message_size, "Recording failed");
        return -1;
    }

    return 0;
}
