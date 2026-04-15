/*
 * recording_profile.h — Pure helpers for recorder sizing and ffmpeg args.
 */

#ifndef VARNISH_RECORDING_PROFILE_H
#define VARNISH_RECORDING_PROFILE_H

#include <stddef.h>

#define VARNISH_RECORDING_FFMPEG_ARGV_MAX    28u
#define VARNISH_RECORDING_FRAMERATE_MAX      16u
#define VARNISH_RECORDING_INPUT_SIZE_MAX     32u
#define VARNISH_RECORDING_FILTER_MAX         96u

typedef struct {
    unsigned framerate;
    int      source_w;
    int      source_h;
    int      output_w;
    int      output_h;
    char     framerate_arg[VARNISH_RECORDING_FRAMERATE_MAX];
    char     input_size_arg[VARNISH_RECORDING_INPUT_SIZE_MAX];
    char     filter_arg[VARNISH_RECORDING_FILTER_MAX];
} varnish_recording_encoder_profile;

int recording_derive_output_size(int source_w, int source_h,
                                 int max_w, int max_h,
                                 int *out_w, int *out_h);
int recording_build_encoder_profile(varnish_recording_encoder_profile *profile,
                                    int source_w, int source_h,
                                    unsigned cadence_ms,
                                    int max_w, int max_h);
size_t recording_build_ffmpeg_argv(
    const varnish_recording_encoder_profile *profile,
    const char *ffmpeg_path,
    const char *output_path,
    const char **out_argv,
    size_t out_argv_cap);
int recording_normalize_wait_status(int status);

#endif /* VARNISH_RECORDING_PROFILE_H */
