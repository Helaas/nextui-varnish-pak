/*
 * recording_shm.c — Daemon-owned shared memory lifecycle for video recording.
 */

#include "recording_shm.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static varnish_recording_shm_t *recording_shm_ptr;
static int recording_shm_fd = -1;

int recording_shm_init(int source_w, int source_h) {
    recording_shm_fd = open(VARNISH_RECORDING_SHM_PATH, O_RDWR | O_CREAT, 0600);
    if (recording_shm_fd < 0) {
        perror("varnish: recording shm open");
        return -1;
    }

    if (fchmod(recording_shm_fd, 0600) < 0) {
        perror("varnish: recording shm chmod");
        close(recording_shm_fd);
        recording_shm_fd = -1;
        return -1;
    }

    if (ftruncate(recording_shm_fd, (off_t)sizeof(varnish_recording_shm_t)) < 0) {
        perror("varnish: recording shm ftruncate");
        close(recording_shm_fd);
        recording_shm_fd = -1;
        return -1;
    }

    recording_shm_ptr = (varnish_recording_shm_t *)mmap(
        NULL, sizeof(varnish_recording_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, recording_shm_fd, 0);
    if (recording_shm_ptr == MAP_FAILED) {
        perror("varnish: recording shm mmap");
        recording_shm_ptr = NULL;
        close(recording_shm_fd);
        recording_shm_fd = -1;
        return -1;
    }

    varnish_recording_reset(recording_shm_ptr, source_w, source_h);
    __sync_synchronize();
    fprintf(stderr, "varnish: recording shm ready at %s (%dx%d)\n",
            VARNISH_RECORDING_SHM_PATH, source_w, source_h);
    return 0;
}

varnish_recording_shm_t *recording_shm_get(void) {
    return recording_shm_ptr;
}

void recording_shm_cleanup(void) {
    if (recording_shm_ptr) {
        varnish_recording_set_active(recording_shm_ptr, 0, 0u, 0, 0);
        munmap(recording_shm_ptr, sizeof(varnish_recording_shm_t));
        recording_shm_ptr = NULL;
    }

    if (recording_shm_fd >= 0) {
        close(recording_shm_fd);
        recording_shm_fd = -1;
    }

    unlink(VARNISH_RECORDING_SHM_PATH);
}
