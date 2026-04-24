/*
 * engine.c - Supervised Multi-Container Runtime (Complete Implementation)
 *
 * Design:
 *   Control plane  : UNIX domain socket at /tmp/mini_runtime.sock
 *   Logging IPC    : pipe per container -> log_reader_thread (producer)
 *                    -> bounded_buffer -> logging_thread (consumer) -> log files
 *   Signal handling: SIGCHLD flag + EINTR on accept(); SIGINT/SIGTERM for shutdown
 *   Namespaces     : CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS per container
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ---- Constants ---- */
#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  256
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)
#define DEFAULT_HARD_LIMIT   (64UL << 20)

/* ---- Enumerations ---- */
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

/* ---- Data Structures ---- */
typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
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
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

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
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* NEW: log reader thread argument (one per container) */
typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} log_reader_arg_t;

/* ---- Global signal flags ---- */
static volatile sig_atomic_t g_sigchld_flag = 0;
static volatile sig_atomic_t g_shutdown_flag = 0;
static supervisor_ctx_t *g_ctx = NULL;

/* ---- Signal handlers ---- */
static void sigchld_handler(int sig) { (void)sig; g_sigchld_flag = 1; }
static void shutdown_handler(int sig) { (void)sig; g_shutdown_flag = 1; }

/* ==========================================================
 *  Usage / Argument Parsing
 * ========================================================== */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *target_bytes)
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
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                 char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ==========================================================
 *  Bounded Buffer  (Task 3)
 * ========================================================== */
static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Producer insert: blocks when buffer is full.
 * Returns -1 if shutdown is in progress.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Consumer remove: blocks when buffer is empty.
 * Returns -1 when shutdown begins and buffer is fully drained.
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ==========================================================
 *  Log Reader Thread  (one per container — PRODUCER side)
 *  Reads raw bytes from pipe, pushes chunks into bounded buffer
 * ========================================================== */
static void *log_reader_thread(void *arg)
{
    log_reader_arg_t *cfg = (log_reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, cfg->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(cfg->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(cfg->log_buffer, &item);
    }

    close(cfg->read_fd);
    free(cfg);
    return NULL;
}

/* ==========================================================
 *  Logging Consumer Thread  (CONSUMER side)
 *  Pops chunks from bounded buffer, appends to per-container log files
 * ========================================================== */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char log_path[PATH_MAX] = "";

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strcmp(c->id, item.container_id) == 0) {
                strncpy(log_path, c->log_path, PATH_MAX - 1);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] != '\0') {
            int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                if (write(fd, item.data, item.length) < 0)
                    perror("[logger] write");
                close(fd);
            }
        }
    }
    return NULL;
}

/* ==========================================================
 *  Child Process Entrypoint  (Task 1)
 *  Runs inside new namespaces after clone()
 * ========================================================== */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* UTS namespace: give container its own hostname */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    /* Redirect stdout and stderr into the logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Mount namespace: chroot into the container's rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* Mount /proc so PID namespace is visible inside the container */
    mount("proc", "/proc", "proc",
          MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    /* Task 5: Apply nice value for scheduling experiments */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the requested command */
    char *argv_exec[] = { cfg->command, NULL };
    execv(cfg->command, argv_exec);
    perror("execv");
    return 1;
}

/* ==========================================================
 *  Kernel Monitor Registration  (provided, unchanged)
 * ========================================================== */
int register_with_monitor(int monitor_fd, const char *container_id,
                           pid_t host_pid,
                           unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id,
                             pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ==========================================================
 *  Child Reaping  (Task 2 — SIGCHLD)
 * ========================================================== */
static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->state     = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->state = (WTERMSIG(status) == SIGKILL)
                                   ? CONTAINER_KILLED : CONTAINER_STOPPED;
                }
                fprintf(stderr,
                        "[supervisor] Reaped container '%s' pid=%d\n",
                        c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ==========================================================
 *  Container Launch  (Tasks 1 + 3 + 4)
 * ========================================================== */
static pid_t launch_container(supervisor_ctx_t *ctx,
                               const control_request_t *req)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    /* child_config lives on the parent stack; clone() gives child a COW copy */
    child_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg.rootfs,  req->rootfs,       PATH_MAX - 1);
    strncpy(cfg.command, req->command,      CHILD_COMMAND_LEN - 1);
    cfg.nice_value   = req->nice_value;
    cfg.log_write_fd = pipefd[1];

    /* Allocate clone stack (child uses this as its call stack) */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    /* Clone child with new PID, UTS, and mount namespaces */
    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      &cfg);

    close(pipefd[1]); /* parent never writes to the log pipe */
    free(stack);       /* child has its own COW address space; safe to free */

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        return -1;
    }

    mkdir(LOG_DIR, 0755);

    /* Allocate and populate container metadata record */
    container_record_t *record = calloc(1, sizeof(container_record_t));
    if (!record) { kill(pid, SIGKILL); close(pipefd[0]); return -1; }

    strncpy(record->id, req->container_id, CONTAINER_ID_LEN - 1);
    record->host_pid         = pid;
    record->started_at       = time(NULL);
    record->state            = CONTAINER_RUNNING;
    record->soft_limit_bytes = req->soft_limit_bytes;
    record->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(record->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next    = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel memory monitor (Task 4) */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                               req->soft_limit_bytes, req->hard_limit_bytes);

    /* Spawn per-container log reader thread (producer) */
    log_reader_arg_t *reader_arg = malloc(sizeof(log_reader_arg_t));
    if (reader_arg) {
        memset(reader_arg, 0, sizeof(*reader_arg));
        strncpy(reader_arg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        reader_arg->read_fd    = pipefd[0];
        reader_arg->log_buffer = &ctx->log_buffer;

        pthread_t reader_tid;
        if (pthread_create(&reader_tid, NULL, log_reader_thread, reader_arg) != 0) {
            free(reader_arg);
            close(pipefd[0]);
        } else {
            pthread_detach(reader_tid);
        }
    } else {
        close(pipefd[0]);
    }

    fprintf(stderr, "[supervisor] Container '%s' started pid=%d log=%s\n",
            req->container_id, pid, record->log_path);
    return pid;
}

/* ==========================================================
 *  Supervisor: Handle One Client Request  (Task 2)
 * ========================================================== */
static void handle_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;

    if (read(client_fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        close(client_fd);
        return;
    }
    memset(&resp, 0, sizeof(resp));

    switch (req.kind) {

    /* --- start: background launch --- */
    case CMD_START: {
        pid_t pid = launch_container(ctx, &req);
        if (pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: failed to start container '%s'", req.container_id);
        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Started container '%s' (pid=%d)", req.container_id, pid);
        }
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    /* --- run: foreground launch, wait for completion --- */
    case CMD_RUN: {
        pid_t pid = launch_container(ctx, &req);
        if (pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: failed to start container '%s'", req.container_id);
            write(client_fd, &resp, sizeof(resp));
        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Running container '%s' (pid=%d), waiting...",
                     req.container_id, pid);
            write(client_fd, &resp, sizeof(resp));
            int status;
            waitpid(pid, &status, 0);
            reap_children(ctx);
        }
        break;
    }

    /* --- ps: list all containers with metadata --- */
    case CMD_PS: {
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "OK");
        write(client_fd, &resp, sizeof(resp));

        char line[512];
        snprintf(line, sizeof(line),
                 "%-16s %-8s %-10s %-10s %-10s %-20s\n",
                 "ID", "PID", "STATE", "SOFT(MB)", "HARD(MB)", "STARTED");
        write(client_fd, line, strlen(line));

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            char tbuf[32] = "";
            struct tm *tm_info = localtime(&c->started_at);
            if (tm_info)
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm_info);
            snprintf(line, sizeof(line),
                     "%-16s %-8d %-10s %-10lu %-10lu %-20s\n",
                     c->id, c->host_pid, state_to_string(c->state),
                     c->soft_limit_bytes >> 20,
                     c->hard_limit_bytes >> 20, tbuf);
            write(client_fd, line, strlen(line));
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }

    /* --- logs: stream container log file --- */
    case CMD_LOGS: {
        char log_path[PATH_MAX] = "";
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strcmp(c->id, req.container_id) == 0) {
                strncpy(log_path, c->log_path, PATH_MAX - 1);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0') {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: container '%s' not found", req.container_id);
            write(client_fd, &resp, sizeof(resp));
        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "OK");
            write(client_fd, &resp, sizeof(resp));
            int log_fd = open(log_path, O_RDONLY);
            if (log_fd >= 0) {
                char buf[4096];
                ssize_t nr;
                while ((nr = read(log_fd, buf, sizeof(buf))) > 0)
                    write(client_fd, buf, nr);
                close(log_fd);
            }
        }
        break;
    }

    /* --- stop: send SIGTERM to container --- */
    case CMD_STOP: {
        pid_t target_pid = -1;
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (strcmp(c->id, req.container_id) == 0) {
                target_pid = c->host_pid;
                if (c->state == CONTAINER_RUNNING)
                    c->state = CONTAINER_STOPPED;
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (target_pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "ERROR: container '%s' not found", req.container_id);
        } else {
            kill(target_pid, SIGTERM);
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Sent SIGTERM to container '%s' (pid=%d)",
                     req.container_id, target_pid);
        }
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command");
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    close(client_fd);
}

/* ==========================================================
 *  Supervisor Main Loop  (Tasks 1 + 2 + 3 + 4 + 6)
 * ========================================================== */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sigaction sa;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open kernel monitor device (Task 4) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[warn] /dev/container_monitor unavailable: %s\n",
                strerror(errno));

    mkdir(LOG_DIR, 0755);

    /* Create UNIX domain socket for CLI control plane (Task 2) */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    unlink(CONTROL_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(ctx.server_fd); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); close(ctx.server_fd); return 1;
    }

    /* Install signal handlers (Task 2) */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = shutdown_handler;
    sa.sa_flags   = 0;   /* no SA_RESTART so EINTR wakes the accept loop */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Start the logging consumer thread (Task 3) */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) { errno = rc; perror("pthread_create logger"); return 1; }

    fprintf(stderr, "[supervisor] Ready. rootfs=%s socket=%s\n",
            rootfs, CONTROL_PATH);

    /* ---- Event loop ---- */
    while (!g_shutdown_flag) {
        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            reap_children(&ctx);
        }

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (g_shutdown_flag) break;
            perror("accept");
            break;
        }
        handle_request(&ctx, client_fd);
    }

    /* ---- Orderly shutdown (Task 6) ---- */
    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* SIGTERM all still-running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING)
            kill(c->host_pid, SIGTERM);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Reap all children - no zombies */
    while (waitpid(-1, NULL, 0) > 0);

    /* Drain and shut down logging pipeline */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Unregister from monitor and free all container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        if (ctx.monitor_fd >= 0)
            unregister_from_monitor(ctx.monitor_fd, c->id, c->host_pid);
        free(c);
        c = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    if (ctx.server_fd  >= 0) { close(ctx.server_fd); unlink(CONTROL_PATH); }

    fprintf(stderr, "[supervisor] Clean exit.\n");
    return 0;
}

/* ==========================================================
 *  Client: Send Control Request to Supervisor  (Task 2)
 * ========================================================== */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s. Is it running?\n",
                CONTROL_PATH);
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    if (read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp)) {
        printf("%s\n", resp.message);
        /* PS and LOGS stream extra text after the response header */
        if (req->kind == CMD_PS || req->kind == CMD_LOGS) {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                fwrite(buf, 1, n, stdout);
        }
    }

    close(fd);
    return 0;
}

/* ==========================================================
 *  CLI Command Dispatchers
 * ========================================================== */
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
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
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ==========================================================
 *  Entry Point
 * ========================================================== */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
