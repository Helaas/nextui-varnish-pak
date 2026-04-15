#include "preload_recording_policy.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "recording_policy_tests: %s\n", (msg)); \
        exit(1); \
    } \
} while (0)

static void test_inactive_never_captures(void) {
    CHECK(varnish_should_capture_recording_frame(0, 0u, 100u, 125u) == 0,
          "inactive recording should never capture");
}

static void test_first_active_frame_captures(void) {
    CHECK(varnish_should_capture_recording_frame(1, 0u, 100u, 125u) == 1,
          "first active frame should capture immediately");
}

static void test_cadence_is_enforced(void) {
    CHECK(varnish_should_capture_recording_frame(1, 100u, 200u, 125u) == 0,
          "frames inside cadence should be skipped");
    CHECK(varnish_should_capture_recording_frame(1, 100u, 225u, 125u) == 1,
          "frame at cadence boundary should capture");
}

int main(void) {
    test_inactive_never_captures();
    test_first_active_frame_captures();
    test_cadence_is_enforced();
    puts("recording_policy_tests: ok");
    return 0;
}
