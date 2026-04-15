/*
 * recording_shm.h — Daemon-owned shared memory lifecycle for video recording.
 */

#ifndef VARNISH_RECORDING_SHM_H
#define VARNISH_RECORDING_SHM_H

#include "recording_transport.h"

int recording_shm_init(int source_w, int source_h);
varnish_recording_shm_t *recording_shm_get(void);
void recording_shm_cleanup(void);

#endif /* VARNISH_RECORDING_SHM_H */
