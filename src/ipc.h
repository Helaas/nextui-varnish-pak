/*
 * ipc.h — FIFO-based IPC for Varnish overlay daemon.
 */

#ifndef VARNISH_IPC_H
#define VARNISH_IPC_H

#ifndef VARNISH_FIFO_PATH
#define VARNISH_FIFO_PATH   "/tmp/varnish.fifo"
#endif

#ifndef VARNISH_PID_PATH
#define VARNISH_PID_PATH    "/tmp/varnish.pid"
#endif

typedef enum {
    IPC_CMD_NONE = 0,
    IPC_CMD_PILL,       /* PILL <client_id> <position> <duration_secs> <text...> */
    IPC_CMD_HIDE,       /* HIDE <client_id> */
    IPC_CMD_CLEAR,      /* CLEAR */
    IPC_CMD_QUIT,       /* QUIT */
    IPC_CMD_HOTKEYS_RELOAD,
    IPC_CMD_HOTKEYS_PAUSE,
    IPC_CMD_HOTKEYS_RESUME,
    IPC_CMD_RECORD_START,
    IPC_CMD_RECORD_STOP,
    IPC_CMD_RECORD_TOGGLE,
} ipc_cmd_type_t;

typedef struct {
    ipc_cmd_type_t type;
    char client_id[64];
    char position[32];
    int  duration_secs;
    char text[256];
} ipc_cmd_t;

int  ipc_init(void);
int  ipc_read(ipc_cmd_t *cmd);
void ipc_cleanup(void);
void ipc_write_pid(void);
int  ipc_daemon_running(void);
int  ipc_kill_daemon(void);
int  ipc_send_pill(const char *client_id, const char *position,
                   int duration_secs, const char *text);
int  ipc_send_hide(const char *client_id);
int  ipc_send_clear(void);
int  ipc_hotkeys_reload(void);
int  ipc_hotkeys_pause(void);
int  ipc_hotkeys_resume(void);
int  ipc_record_start(void);
int  ipc_record_stop(void);
int  ipc_record_toggle(void);

#endif /* VARNISH_IPC_H */
