/*
 * recording.h — Daemon-side video recording session management.
 */

#ifndef VARNISH_RECORDING_H
#define VARNISH_RECORDING_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int      initialized;
    int      active;
    int      encoder_fd;
    int      encoder_pid;
    int      encoder_exit_status;
    int      encoder_timed_out;
    int      source_w;
    int      source_h;
    int      output_w;
    int      output_h;
    unsigned cadence_ms;
    unsigned frames_written;
    unsigned drop_count;
    char     output_path[512];
    char     ffmpeg_path[512];
    unsigned encoder_failures;
} varnish_recording_session;

void recording_session_init(varnish_recording_session *session);
int recording_init_transport(int source_w, int source_h);
void recording_shutdown_transport(void);
bool recording_session_active(const varnish_recording_session *session);
int recording_start(varnish_recording_session *session,
                    char *message, size_t message_size);
int recording_stop(varnish_recording_session *session,
                   char *message, size_t message_size);
int recording_toggle(varnish_recording_session *session,
                     char *message, size_t message_size,
                     int *out_started);
int recording_poll(varnish_recording_session *session,
                   char *message, size_t message_size);

#endif /* VARNISH_RECORDING_H */
