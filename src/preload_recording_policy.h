#ifndef VARNISH_PRELOAD_RECORDING_POLICY_H
#define VARNISH_PRELOAD_RECORDING_POLICY_H

#include <stdint.h>

static inline int varnish_should_capture_recording_frame(int active,
                                                         uint32_t last_capture_ms,
                                                         uint32_t now_ms,
                                                         uint32_t cadence_ms) {
    if (!active)
        return 0;
    if (cadence_ms == 0u)
        cadence_ms = 1u;
    if (last_capture_ms == 0u)
        return 1;
    return (uint32_t)(now_ms - last_capture_ms) >= cadence_ms;
}

#endif /* VARNISH_PRELOAD_RECORDING_POLICY_H */
