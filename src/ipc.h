/*
 * ipc.h — FIFO-based IPC for Varnish overlay daemon.
 */

#ifndef VARNISH_IPC_H
#define VARNISH_IPC_H

#define VARNISH_FIFO_PATH   "/tmp/varnish.fifo"
#define VARNISH_PID_PATH    "/tmp/varnish.pid"

typedef enum {
    IPC_CMD_NONE = 0,
    IPC_CMD_PILL,       /* PILL <client_id> <position> <duration_secs> <text...> */
    IPC_CMD_HIDE,       /* HIDE <client_id> */
    IPC_CMD_CLEAR,      /* CLEAR */
    IPC_CMD_QUIT,       /* QUIT */
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

#endif /* VARNISH_IPC_H */
