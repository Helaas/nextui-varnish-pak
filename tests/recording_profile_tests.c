#include "recording_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "recording_profile_tests: %s\n", (msg)); \
        exit(1); \
    } \
} while (0)

static const char *find_arg_value(const char **argv, size_t argc, const char *flag) {
    for (size_t i = 0; i + 1u < argc; i++) {
        if (strcmp(argv[i], flag) == 0)
            return argv[i + 1u];
    }
    return NULL;
}

static int has_arg(const char **argv, size_t argc, const char *value) {
    for (size_t i = 0; i < argc; i++) {
        if (strcmp(argv[i], value) == 0)
            return 1;
    }
    return 0;
}

static void test_aspect_correct_output_sizes(void) {
    int out_w = 0;
    int out_h = 0;

    CHECK(recording_derive_output_size(1024, 768, 512, 384, &out_w, &out_h) == 0,
          "4:3 output should derive");
    CHECK(out_w == 512 && out_h == 384, "4:3 output should stay 512x384");

    CHECK(recording_derive_output_size(1280, 720, 512, 384, &out_w, &out_h) == 0,
          "16:9 output should derive");
    CHECK(out_w == 512 && out_h == 288, "16:9 output should become 512x288");

    CHECK(recording_derive_output_size(640, 480, 512, 384, &out_w, &out_h) == 0,
          "640x480 output should derive");
    CHECK(out_w == 512 && out_h == 384, "640x480 output should become 512x384");
}

static void test_even_rounding_for_awkward_sizes(void) {
    int out_w = 0;
    int out_h = 0;

    CHECK(recording_derive_output_size(1111, 777, 512, 384, &out_w, &out_h) == 0,
          "awkward source should derive");
    CHECK((out_w % 2) == 0 && (out_h % 2) == 0,
          "derived output should use even dimensions");
    CHECK(out_w <= 512 && out_h <= 384,
          "derived output should stay inside max box");
}

static void test_explicit_ffmpeg_profile(void) {
    varnish_recording_encoder_profile profile;
    const char *argv[VARNISH_RECORDING_FFMPEG_ARGV_MAX];
    const char *vf_value;
    size_t argc;

    CHECK(recording_build_encoder_profile(&profile, 1280, 720, 125u, 512, 384) == 0,
          "encoder profile should build");
    CHECK(profile.output_w == 512 && profile.output_h == 288,
          "encoder profile should preserve aspect ratio");

    argc = recording_build_ffmpeg_argv(&profile, "/tmp/ffmpeg", "/tmp/out.avi",
                                       argv, VARNISH_RECORDING_FFMPEG_ARGV_MAX);
    CHECK(argc > 0u, "ffmpeg argv should build");
    CHECK(strcmp(argv[0], "/tmp/ffmpeg") == 0, "ffmpeg argv should start with path");
    CHECK(has_arg(argv, argc, "-nostdin"), "ffmpeg argv should disable stdin");
    CHECK(has_arg(argv, argc, "-q:v"), "ffmpeg argv should set mjpeg quality");
    CHECK(strcmp(find_arg_value(argv, argc, "-q:v"), "10") == 0,
          "ffmpeg argv should use q:v 10");
    CHECK(strcmp(find_arg_value(argv, argc, "-pixel_format"), "bgra") == 0,
          "ffmpeg argv should use bgra input");
    CHECK(strcmp(find_arg_value(argv, argc, "-video_size"), "1280x720") == 0,
          "ffmpeg argv should describe source size");
    vf_value = find_arg_value(argv, argc, "-vf");
    CHECK(vf_value != NULL, "ffmpeg argv should provide a filter");
    CHECK(strcmp(vf_value, "scale=512:288:flags=bilinear,format=yuvj420p") == 0,
          "ffmpeg argv should use aspect-correct scale plus yuvj420p");
    CHECK(!has_arg(argv, argc, "vflip"), "ffmpeg argv should not include vflip");
}

int main(void) {
    test_aspect_correct_output_sizes();
    test_even_rounding_for_awkward_sizes();
    test_explicit_ffmpeg_profile();
    puts("recording_profile_tests: ok");
    return 0;
}
