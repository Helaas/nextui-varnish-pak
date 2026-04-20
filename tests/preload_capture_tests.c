#include "preload_capture_policy.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CAPTURE_MIN_MS 48u

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "preload_capture_tests: %s\n", (msg));         \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static void test_first_active_frame_captures_immediately(void) {
    void *renderer = (void *)0x1234;

    CHECK(varnish_should_capture_background(
              0, NULL, 0, 0, 0, 0, 0,
              renderer, 10, 20, 30, 40, 100, CAPTURE_MIN_MS) == 1,
          "first active frame should capture immediately");
}

static void test_repeated_presents_inside_threshold_are_skipped(void) {
    void *renderer = (void *)0x1234;

    CHECK(varnish_should_capture_background(
              1, renderer, 10, 20, 30, 40, 100,
              renderer, 10, 20, 30, 40, 120, CAPTURE_MIN_MS) == 0,
          "repeated presents inside threshold should skip capture");
    CHECK(varnish_should_capture_background(
              1, renderer, 10, 20, 30, 40, 100,
              renderer, 10, 20, 30, 40, 148, CAPTURE_MIN_MS) == 1,
          "threshold expiry should allow capture");
}

static void test_renderer_change_forces_capture(void) {
    void *renderer_a = (void *)0x1234;
    void *renderer_b = (void *)0x5678;

    CHECK(varnish_should_capture_background(
              1, renderer_a, 10, 20, 30, 40, 100,
              renderer_b, 10, 20, 30, 40, 101, CAPTURE_MIN_MS) == 1,
          "renderer change should force capture");
}

static void test_geometry_change_forces_capture(void) {
    void *renderer = (void *)0x1234;

    CHECK(varnish_should_capture_background(
              1, renderer, 10, 20, 30, 40, 100,
              renderer, 11, 20, 30, 40, 101, CAPTURE_MIN_MS) == 1,
          "x change should force capture");
    CHECK(varnish_should_capture_background(
              1, renderer, 10, 20, 30, 40, 100,
              renderer, 10, 21, 30, 40, 101, CAPTURE_MIN_MS) == 1,
          "y change should force capture");
    CHECK(varnish_should_capture_background(
              1, renderer, 10, 20, 30, 40, 100,
              renderer, 10, 20, 31, 40, 101, CAPTURE_MIN_MS) == 1,
          "width change should force capture");
    CHECK(varnish_should_capture_background(
              1, renderer, 10, 20, 30, 40, 100,
              renderer, 10, 20, 30, 41, 101, CAPTURE_MIN_MS) == 1,
          "height change should force capture");
}

static void test_inactive_slot_reset_forces_fresh_capture(void) {
    void *renderer = (void *)0x1234;

    CHECK(varnish_should_capture_background(
              1, renderer, 10, 20, 30, 40, 100,
              renderer, 10, 20, 30, 40, 120, CAPTURE_MIN_MS) == 0,
          "baseline should skip within threshold");
    CHECK(varnish_should_capture_background(
              0, NULL, 0, 0, 0, 0, 0,
              renderer, 10, 20, 30, 40, 120, CAPTURE_MIN_MS) == 1,
          "inactive reset should force immediate fresh capture");
}

static void test_retry_deadline_policy(void) {
    CHECK(varnish_retry_deadline_reached(100, 0) == 1,
          "zero deadline should allow immediate retry");
    CHECK(varnish_retry_deadline_reached(100, 101) == 0,
          "future deadline should block retry");
    CHECK(varnish_retry_deadline_reached(148, 148) == 1,
          "equal deadline should allow retry");
    CHECK(varnish_retry_deadline_reached(5, UINT32_MAX - 5) == 1,
          "wraparound deadline should still compare correctly");
}

int main(void) {
    test_first_active_frame_captures_immediately();
    test_repeated_presents_inside_threshold_are_skipped();
    test_renderer_change_forces_capture();
    test_geometry_change_forces_capture();
    test_inactive_slot_reset_forces_fresh_capture();
    test_retry_deadline_policy();
    puts("preload_capture_tests: ok");
    return 0;
}
