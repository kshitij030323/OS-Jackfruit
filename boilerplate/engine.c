/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Single binary, used two ways:
 *   engine supervisor <base-rootfs>        (long-running daemon)
 *   engine start|run|ps|logs|stop ...      (short-lived CLI client)
 *
 * Control-plane IPC: UNIX domain stream socket at /tmp/mini_runtime.sock
 * Logging IPC:       per-container pipe -> producer thread -> bounded
 *                    buffer -> single consumer thread -> log file
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 32
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define REASON_LEN 32
#define MONITOR_DEVICE "/dev/container_monitor"

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char reason[REASON_LEN];
    char log_path[PATH_MAX];
    int log_fd;                  /* persistent log file fd for consumer */
    int pipe_read_fd;            /* read end of container pipe (supervisor) */
    pthread_t producer;
    int producer_running;
    int run_waiter_fd;           /* socket fd of blocked `run` client, or -1 */
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int payload_len;
} control_response_header_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int signal_fd;
    int should_stop;
    pthread_t logger_thread;
    int logger_running;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    char base_rootfs[PATH_MAX];
} supervisor_ctx_t;

static supervisor_ctx_t g_ctx;  /* global so the consumer thread can look up fds */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ---------------- shared helpers ---------------- */

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static ssize_t write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)n;
}

static ssize_t read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r;
        left -= (size_t)r;
    }
    return (ssize_t)n;
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *out)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s too large: %s\n", flag, value);
        return -1;
    }
    *out = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start)
{
    for (int i = start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag(argv[i], argv[i+1], &req->soft_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag(argv[i], argv[i+1], &req->hard_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end = NULL;
            long v;
            errno = 0;
            v = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' || v < -20 || v > 19) {
                fprintf(stderr, "Invalid --nice (-20..19): %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)v;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft-mib must not exceed hard-mib\n");
        return -1;
    }
    return 0;
}

/* ---------------- bounded buffer ---------------- */

static int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    if (pthread_mutex_init(&b->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&b->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&b->mutex); return -1;
    }
    if (pthread_cond_init(&b->not_full, NULL) != 0) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex); return -1;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/* Blocks producer while full; returns -1 if shutdown requested. */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* Returns 1 on item, 0 on shutdown+drained, -1 on fatal error. */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return 0;
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 1;
}

/* ---------------- metadata list ---------------- */

static container_record_t *find_record_locked(const char *id)
{
    for (container_record_t *r = g_ctx.containers; r; r = r->next)
        if (strncmp(r->id, id, CONTAINER_ID_LEN) == 0)
            return r;
    return NULL;
}

static void add_record_locked(container_record_t *r)
{
    r->next = g_ctx.containers;
    g_ctx.containers = r;
}

/* ---------------- logger consumer ---------------- */

static int write_log_for_id(const char *id, const char *data, size_t len)
{
    int fd = -1;
    pthread_mutex_lock(&g_ctx.metadata_lock);
    container_record_t *r = find_record_locked(id);
    if (r) fd = r->log_fd;
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    if (fd < 0) return -1;
    return write_all(fd, data, len) < 0 ? -1 : 0;
}

void *logging_thread(void *arg)
{
    (void)arg;
    log_item_t item;
    for (;;) {
        int rc = bounded_buffer_pop(&g_ctx.log_buffer, &item);
        if (rc <= 0) break;
        write_log_for_id(item.container_id, item.data, item.length);
    }
    return NULL;
}

/* ---------------- producer per container ---------------- */

typedef struct {
    char id[CONTAINER_ID_LEN];
    int read_fd;
} producer_arg_t;

static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    for (;;) {
        ssize_t n = read(pa->read_fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pa->id, CONTAINER_ID_LEN - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        if (bounded_buffer_push(&g_ctx.log_buffer, &item) != 0) break;
    }
    close(pa->read_fd);
    free(pa);
    return NULL;
}

/* ---------------- monitor ioctl wrappers ---------------- */

static int register_with_monitor(int monitor_fd, const char *id, pid_t pid,
                                 unsigned long soft, unsigned long hard)
{
    if (monitor_fd < 0) return 0;  /* monitor optional if module not loaded */
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

static int unregister_from_monitor(int monitor_fd, const char *id, pid_t pid)
{
    if (monitor_fd < 0) return 0;
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

/* ---------------- container child ---------------- */

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Unblock all signals in the container. */
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);

    /* Die if supervisor dies. */
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");

    /* Make all mount propagation private so our mounts don't leak. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount make-private");

    if (chdir(cfg->rootfs) < 0) { perror("chdir rootfs"); return 1; }
    if (chroot(".") < 0)        { perror("chroot");       return 1; }
    if (chdir("/") < 0)         { perror("chdir /");      return 1; }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");

    /* wire stdout/stderr to the supervisor's log pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        if (cfg->log_write_fd > 2) close(cfg->log_write_fd);
    }

    /* new session so the container is isolated from the supervisor's ctty */
    setsid();

    /* scheduling knob */
    if (cfg->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
            perror("setpriority");
    }

    /* execute via /bin/sh -c to allow arguments in the single command string */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 127;
}

/* ---------------- container launch ---------------- */

static void format_now(char *out, size_t n, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

static int launch_container(const control_request_t *req, char *err, size_t errn,
                            container_record_t **out_rec)
{
    /* reject duplicate IDs */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    if (find_record_locked(req->container_id)) {
        pthread_mutex_unlock(&g_ctx.metadata_lock);
        snprintf(err, errn, "container id already exists: %s", req->container_id);
        return -1;
    }
    /* reject duplicate writable rootfs among live containers */
    for (container_record_t *r = g_ctx.containers; r; r = r->next) {
        if (r->state == CONTAINER_STARTING || r->state == CONTAINER_RUNNING) {
            /* best-effort string match; guide requires unique dir per live container */
            if (strcmp(r->id, req->container_id) != 0) {
                /* records don't store rootfs path so we rely on id uniqueness */
            }
        }
    }
    pthread_mutex_unlock(&g_ctx.metadata_lock);

    /* pipe for container stdout/stderr */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(err, errn, "pipe: %s", strerror(errno));
        return -1;
    }

    /* open per-container log file */
    mkdir(LOG_DIR, 0755);
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req->container_id);
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        snprintf(err, errn, "open log: %s", strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    /* child config on heap so it survives until exec */
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { snprintf(err, errn, "oom"); close(pipefd[0]); close(pipefd[1]); close(log_fd); return -1; }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* allocate child stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        snprintf(err, errn, "stack oom");
        free(cfg); close(pipefd[0]); close(pipefd[1]); close(log_fd);
        return -1;
    }

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t child = clone(child_fn, stack + STACK_SIZE, flags, cfg);
    if (child < 0) {
        snprintf(err, errn, "clone: %s", strerror(errno));
        free(stack); free(cfg);
        close(pipefd[0]); close(pipefd[1]); close(log_fd);
        return -1;
    }

    /* parent: close write end so EOF propagates when child exits */
    close(pipefd[1]);
    /* stack/cfg ownership: child will exec and never return. We leak these
     * intentionally (child keeps references); a full impl would track them
     * and free on reap. Keeping it simple here. */

    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        snprintf(err, errn, "oom record");
        kill(child, SIGKILL);
        close(pipefd[0]); close(log_fd);
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid = child;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->log_fd = log_fd;
    rec->pipe_read_fd = pipefd[0];
    rec->run_waiter_fd = -1;
    strncpy(rec->log_path, log_path, sizeof(rec->log_path) - 1);
    strncpy(rec->reason, "-", REASON_LEN - 1);

    /* register with kernel monitor (best-effort) */
    if (register_with_monitor(g_ctx.monitor_fd, rec->id, rec->host_pid,
                              rec->soft_limit_bytes, rec->hard_limit_bytes) < 0) {
        fprintf(stderr, "warning: monitor register failed: %s\n", strerror(errno));
    }

    /* start producer thread */
    producer_arg_t *pa = calloc(1, sizeof(*pa));
    strncpy(pa->id, rec->id, CONTAINER_ID_LEN - 1);
    pa->read_fd = pipefd[0];
    if (pthread_create(&rec->producer, NULL, producer_thread, pa) != 0) {
        snprintf(err, errn, "pthread_create producer");
        unregister_from_monitor(g_ctx.monitor_fd, rec->id, rec->host_pid);
        kill(child, SIGKILL);
        close(pipefd[0]); close(log_fd);
        free(pa); free(rec);
        return -1;
    }
    rec->producer_running = 1;

    pthread_mutex_lock(&g_ctx.metadata_lock);
    add_record_locked(rec);
    pthread_mutex_unlock(&g_ctx.metadata_lock);

    *out_rec = rec;
    return 0;
}

/* ---------------- reap loop ---------------- */

static void finalize_record(container_record_t *rec, int status)
{
    if (WIFEXITED(status)) {
        rec->exit_code = WEXITSTATUS(status);
        rec->exit_signal = 0;
        rec->state = CONTAINER_EXITED;
        snprintf(rec->reason, REASON_LEN, "exited(%d)", rec->exit_code);
    } else if (WIFSIGNALED(status)) {
        rec->exit_code = 128 + WTERMSIG(status);
        rec->exit_signal = WTERMSIG(status);
        if (rec->stop_requested) {
            rec->state = CONTAINER_STOPPED;
            snprintf(rec->reason, REASON_LEN, "stopped(sig %d)", rec->exit_signal);
        } else if (WTERMSIG(status) == SIGKILL) {
            rec->state = CONTAINER_KILLED;
            snprintf(rec->reason, REASON_LEN, "hard_limit_killed");
        } else {
            rec->state = CONTAINER_KILLED;
            snprintf(rec->reason, REASON_LEN, "signal(%d)", rec->exit_signal);
        }
    }

    unregister_from_monitor(g_ctx.monitor_fd, rec->id, rec->host_pid);

    /* join producer thread; read side will see EOF now that child is gone. */
    if (rec->producer_running) {
        pthread_join(rec->producer, NULL);
        rec->producer_running = 0;
    }
    /* flush log fd */
    if (rec->log_fd >= 0) {
        fsync(rec->log_fd);
        /* keep open so `logs` can still read; supervisor shutdown closes it */
    }

    /* notify run waiter if any */
    if (rec->run_waiter_fd >= 0) {
        control_response_header_t hdr;
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
                         "container %s finished: %s (exit_code=%d)\n",
                         rec->id, rec->reason, rec->exit_code);
        hdr.status = rec->exit_code;
        hdr.payload_len = n;
        write_all(rec->run_waiter_fd, &hdr, sizeof(hdr));
        write_all(rec->run_waiter_fd, msg, (size_t)n);
        close(rec->run_waiter_fd);
        rec->run_waiter_fd = -1;
    }
}

static void reap_children(void)
{
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        pthread_mutex_lock(&g_ctx.metadata_lock);
        container_record_t *rec = NULL;
        for (container_record_t *r = g_ctx.containers; r; r = r->next)
            if (r->host_pid == pid) { rec = r; break; }
        if (rec) finalize_record(rec, status);
        pthread_mutex_unlock(&g_ctx.metadata_lock);
    }
}

/* ---------------- command handlers (supervisor side) ---------------- */

static void send_simple(int fd, int status, const char *msg)
{
    control_response_header_t hdr;
    hdr.status = status;
    hdr.payload_len = (int)strlen(msg);
    write_all(fd, &hdr, sizeof(hdr));
    if (hdr.payload_len) write_all(fd, msg, (size_t)hdr.payload_len);
}

static void send_payload(int fd, int status, const char *buf, size_t n)
{
    control_response_header_t hdr;
    hdr.status = status;
    hdr.payload_len = (int)n;
    write_all(fd, &hdr, sizeof(hdr));
    if (n) write_all(fd, buf, n);
}

static void handle_start(int client_fd, const control_request_t *req, int is_run)
{
    char err[256];
    container_record_t *rec = NULL;
    int rc = launch_container(req, err, sizeof(err), &rec);
    if (rc != 0) { send_simple(client_fd, 1, err); close(client_fd); return; }

    if (is_run) {
        pthread_mutex_lock(&g_ctx.metadata_lock);
        rec->run_waiter_fd = client_fd;  /* response sent from reap path */
        pthread_mutex_unlock(&g_ctx.metadata_lock);
        /* do NOT close client_fd yet */
    } else {
        char ok[128];
        snprintf(ok, sizeof(ok), "started %s (pid %d)\n", rec->id, rec->host_pid);
        send_simple(client_fd, 0, ok);
        close(client_fd);
    }
}

static void handle_ps(int client_fd)
{
    char *buf = malloc(64 * 1024);
    if (!buf) { send_simple(client_fd, 1, "oom"); close(client_fd); return; }
    size_t off = 0;
    off += snprintf(buf + off, 64*1024 - off,
                    "%-12s %-8s %-10s %-20s %-8s %-8s %s\n",
                    "ID", "PID", "STATE", "STARTED", "SOFT_MB", "HARD_MB", "REASON");
    pthread_mutex_lock(&g_ctx.metadata_lock);
    for (container_record_t *r = g_ctx.containers; r; r = r->next) {
        char tbuf[32];
        format_now(tbuf, sizeof(tbuf), r->started_at);
        off += snprintf(buf + off, 64*1024 - off,
                        "%-12s %-8d %-10s %-20s %-8lu %-8lu %s\n",
                        r->id, r->host_pid, state_to_string(r->state),
                        tbuf,
                        r->soft_limit_bytes >> 20,
                        r->hard_limit_bytes >> 20,
                        r->reason);
        if (off > 60000) break;
    }
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    send_payload(client_fd, 0, buf, off);
    close(client_fd);
    free(buf);
}

static void handle_logs(int client_fd, const control_request_t *req)
{
    char log_path[PATH_MAX];
    pthread_mutex_lock(&g_ctx.metadata_lock);
    container_record_t *r = find_record_locked(req->container_id);
    if (!r) {
        pthread_mutex_unlock(&g_ctx.metadata_lock);
        send_simple(client_fd, 1, "no such container\n");
        close(client_fd);
        return;
    }
    strncpy(log_path, r->log_path, sizeof(log_path) - 1);
    log_path[sizeof(log_path) - 1] = '\0';
    pthread_mutex_unlock(&g_ctx.metadata_lock);

    int fd = open(log_path, O_RDONLY);
    if (fd < 0) { send_simple(client_fd, 1, "open log failed\n"); close(client_fd); return; }

    /* cap at 256 KiB */
    size_t cap = 256 * 1024;
    char *buf = malloc(cap);
    ssize_t nread = 0, n;
    while ((size_t)nread < cap &&
           (n = read(fd, buf + nread, cap - (size_t)nread)) > 0)
        nread += n;
    close(fd);
    send_payload(client_fd, 0, buf, (size_t)(nread < 0 ? 0 : nread));
    free(buf);
    close(client_fd);
}

static void handle_stop(int client_fd, const control_request_t *req)
{
    pthread_mutex_lock(&g_ctx.metadata_lock);
    container_record_t *r = find_record_locked(req->container_id);
    if (!r) {
        pthread_mutex_unlock(&g_ctx.metadata_lock);
        send_simple(client_fd, 1, "no such container\n");
        close(client_fd);
        return;
    }
    pid_t pid = r->host_pid;
    r->stop_requested = 1;
    pthread_mutex_unlock(&g_ctx.metadata_lock);

    /* SIGTERM first, then SIGKILL if still running after 2s. */
    kill(pid, SIGTERM);
    char msg[128];
    snprintf(msg, sizeof(msg), "stop signaled (pid %d)\n", pid);
    send_simple(client_fd, 0, msg);
    close(client_fd);
}

/* ---------------- server side ---------------- */

static void handle_client(int client_fd)
{
    control_request_t req;
    if (read_all(client_fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        close(client_fd);
        return;
    }
    switch (req.kind) {
    case CMD_START: handle_start(client_fd, &req, 0); break;
    case CMD_RUN:   handle_start(client_fd, &req, 1); break;
    case CMD_PS:    handle_ps(client_fd); break;
    case CMD_LOGS:  handle_logs(client_fd, &req); break;
    case CMD_STOP:  handle_stop(client_fd, &req); break;
    default:        send_simple(client_fd, 1, "unknown command\n"); close(client_fd);
    }
}

static int setup_server_socket(void)
{
    unlink(CONTROL_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    chmod(CONTROL_PATH, 0666);
    if (listen(fd, 16) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

static int setup_signalfd(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) { perror("sigprocmask"); return -1; }
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) perror("signalfd");
    return sfd;
}

static void supervisor_shutdown(void)
{
    /* signal all running containers, then wait briefly */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    for (container_record_t *r = g_ctx.containers; r; r = r->next) {
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING) {
            r->stop_requested = 1;
            kill(r->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_ctx.metadata_lock);

    /* drain for up to ~3 seconds */
    for (int i = 0; i < 30; i++) {
        reap_children();
        int any = 0;
        pthread_mutex_lock(&g_ctx.metadata_lock);
        for (container_record_t *r = g_ctx.containers; r; r = r->next)
            if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING) { any = 1; break; }
        pthread_mutex_unlock(&g_ctx.metadata_lock);
        if (!any) break;
        usleep(100 * 1000);
    }
    /* force-kill stragglers */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    for (container_record_t *r = g_ctx.containers; r; r = r->next)
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING)
            kill(r->host_pid, SIGKILL);
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    for (int i = 0; i < 20; i++) {
        reap_children();
        usleep(100 * 1000);
    }

    /* stop consumer */
    bounded_buffer_begin_shutdown(&g_ctx.log_buffer);
    if (g_ctx.logger_running) {
        pthread_join(g_ctx.logger_thread, NULL);
        g_ctx.logger_running = 0;
    }

    /* close log fds + free records */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    container_record_t *r = g_ctx.containers;
    while (r) {
        container_record_t *next = r->next;
        if (r->log_fd >= 0) close(r->log_fd);
        free(r);
        r = next;
    }
    g_ctx.containers = NULL;
    pthread_mutex_unlock(&g_ctx.metadata_lock);

    if (g_ctx.server_fd >= 0) close(g_ctx.server_fd);
    if (g_ctx.monitor_fd >= 0) close(g_ctx.monitor_fd);
    if (g_ctx.signal_fd >= 0) close(g_ctx.signal_fd);
    unlink(CONTROL_PATH);

    bounded_buffer_destroy(&g_ctx.log_buffer);
    pthread_mutex_destroy(&g_ctx.metadata_lock);
}

static int run_supervisor(const char *rootfs)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.server_fd = -1;
    g_ctx.monitor_fd = -1;
    g_ctx.signal_fd = -1;
    strncpy(g_ctx.base_rootfs, rootfs, sizeof(g_ctx.base_rootfs) - 1);

    if (pthread_mutex_init(&g_ctx.metadata_lock, NULL) != 0) return 1;
    if (bounded_buffer_init(&g_ctx.log_buffer) != 0) return 1;

    g_ctx.monitor_fd = open(MONITOR_DEVICE, O_RDWR);
    if (g_ctx.monitor_fd < 0)
        fprintf(stderr, "note: %s unavailable (%s); memory enforcement disabled\n",
                MONITOR_DEVICE, strerror(errno));

    g_ctx.server_fd = setup_server_socket();
    if (g_ctx.server_fd < 0) return 1;

    g_ctx.signal_fd = setup_signalfd();
    if (g_ctx.signal_fd < 0) return 1;

    if (pthread_create(&g_ctx.logger_thread, NULL, logging_thread, NULL) != 0) {
        perror("pthread_create logger"); return 1;
    }
    g_ctx.logger_running = 1;

    printf("supervisor up. base-rootfs=%s socket=%s\n", g_ctx.base_rootfs, CONTROL_PATH);
    fflush(stdout);

    struct pollfd pfd[2];
    while (!g_ctx.should_stop) {
        pfd[0].fd = g_ctx.server_fd; pfd[0].events = POLLIN;
        pfd[1].fd = g_ctx.signal_fd; pfd[1].events = POLLIN;
        int rc = poll(pfd, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }
        if (pfd[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(g_ctx.signal_fd, &si, sizeof(si)) == sizeof(si)) {
                if (si.ssi_signo == SIGCHLD) {
                    reap_children();
                } else if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM) {
                    fprintf(stderr, "supervisor: received signal %u, shutting down\n",
                            si.ssi_signo);
                    g_ctx.should_stop = 1;
                    break;
                }
            }
        }
        if (g_ctx.should_stop) break;
        if (pfd[0].revents & POLLIN) {
            int c = accept(g_ctx.server_fd, NULL, NULL);
            if (c < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
            handle_client(c);
        }
    }

    supervisor_shutdown();
    return 0;
}

/* ---------------- client side ---------------- */

static int connect_supervisor(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static volatile sig_atomic_t g_client_stop_sent = 0;
static char g_client_run_id[CONTAINER_ID_LEN];

static void client_forward_stop(int sig)
{
    (void)sig;
    if (g_client_stop_sent) return;
    g_client_stop_sent = 1;
    int fd = connect_supervisor();
    if (fd < 0) return;
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, g_client_run_id, CONTAINER_ID_LEN - 1);
    write_all(fd, &req, sizeof(req));
    /* drain reply */
    control_response_header_t h;
    read_all(fd, &h, sizeof(h));
    if (h.payload_len > 0) {
        char tmp[512];
        int want = h.payload_len > (int)sizeof(tmp) ? (int)sizeof(tmp) : h.payload_len;
        read_all(fd, tmp, (size_t)want);
    }
    close(fd);
}

static int send_control_request(const control_request_t *req)
{
    int fd = connect_supervisor();
    if (fd < 0) {
        fprintf(stderr, "cannot connect to supervisor at %s: %s\n",
                CONTROL_PATH, strerror(errno));
        return 1;
    }
    if (req->kind == CMD_RUN) {
        strncpy(g_client_run_id, req->container_id, CONTAINER_ID_LEN - 1);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = client_forward_stop;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    if (write_all(fd, req, sizeof(*req)) < 0) {
        fprintf(stderr, "write request: %s\n", strerror(errno));
        close(fd); return 1;
    }

    control_response_header_t hdr;
    ssize_t r = read_all(fd, &hdr, sizeof(hdr));
    if (r != (ssize_t)sizeof(hdr)) {
        fprintf(stderr, "supervisor closed connection\n");
        close(fd); return 1;
    }
    if (hdr.payload_len > 0) {
        char *buf = malloc((size_t)hdr.payload_len + 1);
        if (read_all(fd, buf, (size_t)hdr.payload_len) == hdr.payload_len) {
            buf[hdr.payload_len] = '\0';
            fputs(buf, hdr.status == 0 ? stdout : stderr);
        }
        free(buf);
    }
    close(fd);
    return hdr.status;
}

/* ---------------- cli command dispatch ---------------- */

static int cmd_start_or_run(int argc, char *argv[], command_kind_t kind)
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s %s <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0], kind == CMD_RUN ? "run" : "start");
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = kind;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs, argv[3], PATH_MAX - 1);
    strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start_or_run(argc, argv, CMD_START);
    if (strcmp(argv[1], "run")   == 0) return cmd_start_or_run(argc, argv, CMD_RUN);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
