/*
 * manual.h — ScrapeGoat manual lookup and SDLReader handoff helpers.
 */

#ifndef VARNISH_MANUAL_H
#define VARNISH_MANUAL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define VARNISH_MANUAL_PATH_MAX 512
#define VARNISH_MANUAL_TAG_MAX  64
#define VARNISH_MANUAL_NAME_MAX 256

typedef struct {
    char manual_root[VARNISH_MANUAL_PATH_MAX];
    char system_tag[VARNISH_MANUAL_TAG_MAX];
    char display_name[VARNISH_MANUAL_NAME_MAX];
    char manual_path[VARNISH_MANUAL_PATH_MAX];
    char browse_dir[VARNISH_MANUAL_PATH_MAX];
    bool exact_match;
} varnish_manual_lookup;

typedef struct {
    pid_t minarch_pid;
    pid_t reader_pid;
    bool active;
    bool minarch_stopped;
    char temp_state_dir[VARNISH_MANUAL_PATH_MAX];
} varnish_manual_session;

void manual_lookup_init(varnish_manual_lookup *lookup);
int manual_read_download_dir(char *out, size_t out_size);
int manual_parse_minarch_cmdline(const char *cmdline, size_t cmdline_len,
                                 char *out_rom_path, size_t out_rom_size);
int manual_find_active_minarch(pid_t *out_pid,
                               char *out_rom_path, size_t out_rom_size);
int manual_build_lookup_from_rom(const char *rom_path,
                                 const char *manual_root,
                                 varnish_manual_lookup *out);
int manual_prepare_browser_state(const char *source_state_dir,
                                 const char *browse_dir,
                                 char *out_state_dir, size_t out_state_dir_size);
void manual_cleanup_temp_state(const char *state_dir);

void manual_session_init(varnish_manual_session *session);
int manual_session_begin(varnish_manual_session *session,
                         pid_t minarch_pid,
                         pid_t reader_pid,
                         const char *temp_state_dir);
int manual_session_poll(varnish_manual_session *session);
void manual_session_abort(varnish_manual_session *session);

int manual_start_for_active_game(varnish_manual_session *session,
                                 char *message, size_t message_size);

#endif /* VARNISH_MANUAL_H */
