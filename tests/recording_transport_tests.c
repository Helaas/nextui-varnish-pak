/*
 * recording_transport_tests.c — Host-side tests for recorder shared transport.
 */

#include "recording_transport.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

#define CHECK(cond, msg) do { \
    if (!(cond)) fail(msg); \
} while (0)

static void test_roundtrip_frame(void) {
    static varnish_recording_shm_t shm;
    varnish_recording_frame_info info;
    uint32_t pixels[16];
    uint32_t *dst;
    uint32_t slot_index = 0u;
    uint32_t out[16];
    int rc;

    varnish_recording_reset(&shm, 1024, 768);
    varnish_recording_set_active(&shm, 1, 125u, 512, 384);
    CHECK(varnish_recording_try_acquire_producer(&shm, 42, 100u),
          "producer should acquire lease");

    dst = varnish_recording_begin_frame(&shm, 42, 4, 4, &slot_index, &info);
    CHECK(dst != NULL, "begin_frame should reserve a slot");

    for (int i = 0; i < 16; i++) {
        pixels[i] = 0xFF000000u | (uint32_t)i;
        dst[i] = pixels[i];
    }
    varnish_recording_publish_frame(&shm, slot_index);

    memset(out, 0, sizeof(out));
    memset(&info, 0, sizeof(info));
    rc = varnish_recording_pop_frame(&shm, out, 16, &info);
    CHECK(rc == 1, "pop should succeed");
    CHECK(info.frame_id == 1u, "first frame should use frame id 1");
    CHECK(info.producer_pid == 42, "producer pid should roundtrip");
    CHECK(info.width == 4 && info.height == 4, "frame geometry should roundtrip");
    CHECK(memcmp(out, pixels, sizeof(out)) == 0, "frame pixels should roundtrip");
    CHECK(varnish_recording_pop_frame(&shm, out, 16, &info) == 0,
          "queue should be empty after pop");
}

static void test_ring_full_drops(void) {
    static varnish_recording_shm_t shm;

    varnish_recording_reset(&shm, 1024, 768);
    varnish_recording_set_active(&shm, 1, 125u, 512, 384);
    CHECK(varnish_recording_try_acquire_producer(&shm, 7, 100u),
          "producer should acquire lease");

    for (int i = 0; i < VARNISH_RECORDING_RING_SLOTS - 1; i++) {
        uint32_t slot_index = 0u;
        uint32_t *dst = varnish_recording_begin_frame(&shm, 7, 1, 1, &slot_index, NULL);
        CHECK(dst != NULL, "ring should accept up to capacity");
        dst[0] = (uint32_t)i;
        varnish_recording_publish_frame(&shm, slot_index);
    }

    CHECK(varnish_recording_begin_frame(&shm, 7, 1, 1, NULL, NULL) == NULL,
          "ring should drop when full");
    CHECK(shm.drop_count == 1u, "full ring should increment drop count");
}

static void test_producer_lease_handoff(void) {
    static varnish_recording_shm_t shm;

    varnish_recording_reset(&shm, 1024, 768);
    varnish_recording_set_active(&shm, 1, 125u, 512, 384);

    CHECK(varnish_recording_try_acquire_producer(&shm, 11, 100u),
          "first producer should acquire lease");
    CHECK(!varnish_recording_try_acquire_producer(&shm, 12, 200u),
          "second producer should be blocked during lease");
    CHECK(varnish_recording_try_acquire_producer(
              &shm, 12, 100u + VARNISH_RECORDING_PRODUCER_LEASE_MS + 1u),
          "second producer should acquire after lease expiry");
}

int main(void) {
    test_roundtrip_frame();
    test_ring_full_drops();
    test_producer_lease_handoff();
    puts("recording_transport_tests: ok");
    return 0;
}
