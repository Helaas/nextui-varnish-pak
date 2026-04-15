/*
 * recording_profile.c — Pure helpers for recorder sizing and ffmpeg args.
 */

#include "recording_profile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int recording_even_floor(int value) {
    if (value <= 2)
        return 2;
    return value & ~1;
}

int recording_derive_output_size(int source_w, int source_h,
                                 int max_w, int max_h,
                                 int *out_w, int *out_h) {
    int width;
    int height;

    if (source_w <= 0 || source_h <= 0 || max_w <= 0 || max_h <= 0 ||
        !out_w || !out_h) {
        return -1;
    }

    width = source_w;
    height = source_h;

    if (width > max_w || height > max_h) {
        double scale_w = (double)max_w / (double)width;
        double scale_h = (double)max_h / (double)height;
        double scale = scale_w < scale_h ? scale_w : scale_h;

        width = (int)floor((double)source_w * scale);
        height = (int)floor((double)source_h * scale);
    }

    width = recording_even_floor(width);
    height = recording_even_floor(height);
    if (width > max_w)
        width = recording_even_floor(max_w);
    if (height > max_h)
        height = recording_even_floor(max_h);
    if (width <= 0 || height <= 0)
        return -1;

    *out_w = width;
    *out_h = height;
    return 0;
}

int recording_build_encoder_profile(varnish_recording_encoder_profile *profile,
                                    int source_w, int source_h,
                                    unsigned cadence_ms,
                                    int max_w, int max_h) {
    unsigned framerate;

    if (!profile)
        return -1;

    framerate = cadence_ms ? (1000u / cadence_ms) : 8u;
    if (framerate == 0u)
        framerate = 1u;

    memset(profile, 0, sizeof(*profile));
    profile->framerate = framerate;
    profile->source_w = source_w;
    profile->source_h = source_h;
    if (recording_derive_output_size(source_w, source_h, max_w, max_h,
                                     &profile->output_w, &profile->output_h) != 0) {
        return -1;
    }

    snprintf(profile->framerate_arg, sizeof(profile->framerate_arg), "%u",
             profile->framerate);
    snprintf(profile->input_size_arg, sizeof(profile->input_size_arg), "%dx%d",
             profile->source_w, profile->source_h);
    snprintf(profile->filter_arg, sizeof(profile->filter_arg),
             "scale=%d:%d:flags=bilinear,format=yuvj420p",
             profile->output_w, profile->output_h);
    return 0;
}

size_t recording_build_ffmpeg_argv(
    const varnish_recording_encoder_profile *profile,
    const char *ffmpeg_path,
    const char *output_path,
    const char **out_argv,
    size_t out_argv_cap) {
    size_t argc = 0u;

    if (!profile || !ffmpeg_path || !ffmpeg_path[0] ||
        !output_path || !output_path[0] ||
        !out_argv || out_argv_cap < VARNISH_RECORDING_FFMPEG_ARGV_MAX) {
        return 0u;
    }

    out_argv[argc++] = ffmpeg_path;
    out_argv[argc++] = "-nostdin";
    out_argv[argc++] = "-y";
    out_argv[argc++] = "-hide_banner";
    out_argv[argc++] = "-loglevel";
    out_argv[argc++] = "error";
    out_argv[argc++] = "-f";
    out_argv[argc++] = "rawvideo";
    out_argv[argc++] = "-framerate";
    out_argv[argc++] = profile->framerate_arg;
    out_argv[argc++] = "-pixel_format";
    out_argv[argc++] = "bgra";
    out_argv[argc++] = "-video_size";
    out_argv[argc++] = profile->input_size_arg;
    out_argv[argc++] = "-i";
    out_argv[argc++] = "pipe:0";
    out_argv[argc++] = "-vf";
    out_argv[argc++] = profile->filter_arg;
    out_argv[argc++] = "-c:v";
    out_argv[argc++] = "mjpeg";
    out_argv[argc++] = "-q:v";
    out_argv[argc++] = "10";
    out_argv[argc++] = "-an";
    out_argv[argc++] = "-f";
    out_argv[argc++] = "avi";
    out_argv[argc++] = output_path;
    out_argv[argc] = NULL;
    return argc;
}

int recording_normalize_wait_status(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}
