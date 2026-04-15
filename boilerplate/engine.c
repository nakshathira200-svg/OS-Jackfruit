/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 * Fixed version: SIGPIPE ignore, working `run`, joinable producers, clean shutdown.
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

#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MAX_CONTAINERS      32

typedef enum {
    CMD_SUPERVISOR = 0, CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0, CONTAINER_RUNNING,
    CONTAINER_STOPPED, CONTAINER_KILLED, CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    int                stop_requested;
    char               log_path[PATH_MAX];
    pthread_t          producer_thread;
    int                pipe_read_fd;
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head, tail, count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int                  server_fd;
    int                  monitor_fd;
    volatile int         should_stop;
    pthread_t            logger_thread;
    bounded_buffer_t     log_buffer;
    pthread_mutex_t      metadata_lock;
    container_record_t  *containers;
} supervisor_ctx_t;

typedef struct {
    int               pipe_fd;
    char              container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
    container_record_t *record;
} producer_arg_t;

static supervisor_ctx_t *g_ctx = NULL;

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

static int parse_mib_flag(const char *flag, const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno || end == value || *end) {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                char *argv[], int start)
{
    for (int i = start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes)) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes)) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end; long v = strtol(argv[i+1], &end, 10);
            if (*end || v < -20 || v > 19) {
                fprintf(stderr, "Invalid --nice value: %s\n", argv[i+1]); return -1;
            }
            req->nice_value = (int)v;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]); return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n"); return -1;
    }
    return 0;
}

static int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
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

static int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down) { pthread_mutex_unlock(&b->mutex); return -1; }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex); return -1;
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t r = write(fd, item.data, item.length);
            (void)r;
            close(fd);
        }
    }
    return NULL;
}

static void *producer_thread(void *arg)
{
    producer_arg_t *p = (producer_arg_t *)arg;
    char tmp[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(p->pipe_fd, tmp, sizeof(tmp) - 1)) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, p->container_id, CONTAINER_ID_LEN - 1);
        item.container_id[CONTAINER_ID_LEN-1] = '\0';
        item.length = (size_t)n;
        memcpy(item.data, tmp, (size_t)n);
        bounded_buffer_push(p->buf, &item);
    }
    close(p->pipe_fd);
    free(p);
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    if (cfg->nice_value != 0) nice(cfg->nice_value);

    if (chroot(cfg->rootfs) != 0) { perror("chroot"); return 1; }
    if (chdir("/") != 0)          { perror("chdir"); return 1; }

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    char *child_argv[] = { cfg->command, NULL };
    char *envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root", "TERM=xterm", NULL
    };
    execve(cfg->command, child_argv, envp);
    perror("execve");
    return 1;
}

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

static container_record_t *add_container(supervisor_ctx_t *ctx,
                                          const control_request_t *req)
{
    container_record_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    strncpy(c->id, req->container_id, CONTAINER_ID_LEN - 1);
    c->id[CONTAINER_ID_LEN-1] = '\0';
    strncpy(c->log_path, LOG_DIR, PATH_MAX - 1);
    c->soft_limit_bytes = req->soft_limit_bytes;
    c->hard_limit_bytes = req->hard_limit_bytes;
    c->state            = CONTAINER_STARTING;
    c->started_at       = time(NULL);
    c->stop_requested   = 0;
    c->producer_thread  = 0;
    c->pipe_read_fd     = -1;

    pthread_mutex_lock(&ctx->metadata_lock);
    c->next         = ctx->containers;
    ctx->containers = c;
    pthread_mutex_unlock(&ctx->metadata_lock);
    return c;
}

static pid_t spawn_container(supervisor_ctx_t *ctx,
                              const control_request_t *req,
                              container_record_t *rec)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return -1; }

    char *stack = malloc(STACK_SIZE);
    if (!stack) { close(pipefd[0]); close(pipefd[1]); return -1; }

    child_config_t *cfg = calloc(1, sizeof(child_config_t));
    if (!cfg) { free(stack); close(pipefd[0]); close(pipefd[1]); return -1; }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    cfg->id[CONTAINER_ID_LEN-1] = '\0';
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    cfg->rootfs[PATH_MAX-1] = '\0';
    strncpy(cfg->command, req->command,        CHILD_COMMAND_LEN - 1);
    cfg->command[CHILD_COMMAND_LEN-1] = '\0';
    cfg->nice_value    = req->nice_value;
    cfg->log_write_fd  = pipefd[1];

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);

    close(pipefd[1]);
    free(stack);
    free(cfg);

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        return -1;
    }

    if (ctx->monitor_fd >= 0) {
        struct monitor_request mr;
        memset(&mr, 0, sizeof(mr));
        mr.pid               = pid;
        mr.soft_limit_bytes  = req->soft_limit_bytes;
        mr.hard_limit_bytes  = req->hard_limit_bytes;
        strncpy(mr.container_id, req->container_id, MONITOR_NAME_LEN - 1);
        mr.container_id[MONITOR_NAME_LEN-1] = '\0';
        ioctl(ctx->monitor_fd, MONITOR_REGISTER, &mr);
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->host_pid = pid;
    rec->state    = CONTAINER_RUNNING;
    rec->pipe_read_fd = pipefd[0];
    pthread_mutex_unlock(&ctx->metadata_lock);

    producer_arg_t *pa = malloc(sizeof(*pa));
    if (pa) {
        pa->pipe_fd = pipefd[0];
        pa->buf     = &ctx->log_buffer;
        strncpy(pa->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        pa->container_id[CONTAINER_ID_LEN-1] = '\0';
        pa->record = rec;
        pthread_create(&rec->producer_thread, NULL, producer_thread, pa);
    }

    return pid;
}

static void sigchld_handler(int sig)
{
    (void)sig;
    if (!g_ctx) return;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    if (c->stop_requested)
                        c->state = CONTAINER_STOPPED;
                    else
                        c->state = CONTAINER_KILLED;
                }
                if (g_ctx->monitor_fd >= 0) {
                    struct monitor_request mr;
                    memset(&mr, 0, sizeof(mr));
                    mr.pid = pid;
                    strncpy(mr.container_id, c->id, MONITOR_NAME_LEN - 1);
                    mr.container_id[MONITOR_NAME_LEN-1] = '\0';
                    ioctl(g_ctx->monitor_fd, MONITOR_UNREGISTER, &mr);
                }
                if (c->pipe_read_fd >= 0) {
                    close(c->pipe_read_fd);
                    c->pipe_read_fd = -1;
                }
                if (c->producer_thread) {
                    pthread_join(c->producer_thread, NULL);
                    c->producer_thread = 0;
                }
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

static void handle_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t  req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (read(client_fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "bad request");
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    if (req.kind == CMD_PS) {
        char buf[4096] = "";
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            char line[256];
            snprintf(line, sizeof(line),
                     "%-16s pid=%-6d state=%-10s soft=%luMiB hard=%luMiB\n",
                     c->id, c->host_pid, state_to_string(c->state),
                     c->soft_limit_bytes >> 20, c->hard_limit_bytes >> 20);
            strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "%s", buf);
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    if (req.kind == CMD_LOGS) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
        FILE *f = fopen(path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "no log for %s", req.container_id);
        } else {
            char tmp[CONTROL_MESSAGE_LEN - 1];
            size_t n = fread(tmp, 1, sizeof(tmp) - 1, f);
            tmp[n] = '\0';
            fclose(f);
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "%s", tmp);
        }
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    if (req.kind == CMD_STOP) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c || c->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container %s not running", req.container_id);
            write(client_fd, &resp, sizeof(resp));
            return;
        }
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        kill(pid, SIGTERM);
        usleep(500000);
        if (kill(pid, 0) == 0)
            kill(pid, SIGKILL);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "stopped %s", req.container_id);
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        pthread_mutex_lock(&ctx->metadata_lock);
        if (find_container(ctx, req.container_id)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "id %s already exists", req.container_id);
            write(client_fd, &resp, sizeof(resp));
            return;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        container_record_t *rec = add_container(ctx, &req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "out of memory");
            write(client_fd, &resp, sizeof(resp));
            return;
        }

        pid_t pid = spawn_container(ctx, &req, rec);
        if (pid < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "spawn failed");
            write(client_fd, &resp, sizeof(resp));
            return;
        }

        if (req.kind == CMD_START) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "started %s pid=%d", req.container_id, pid);
            write(client_fd, &resp, sizeof(resp));
            return;
        }

        int status;
        waitpid(pid, &status, 0);
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
        resp.status = exit_code;
        snprintf(resp.message, sizeof(resp.message), "container %s exited with code %d", req.container_id, exit_code);
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    resp.status = -1;
    snprintf(resp.message, sizeof(resp.message), "unknown command");
    write(client_fd, &resp, sizeof(resp));
}

static int run_supervisor(const char *rootfs)
{
    (void)rootfs;
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);

    mkdir(LOG_DIR, 0755);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] kernel monitor not available: %s\n",
                strerror(errno));

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) { perror("listen"); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags   = SA_RESTART;
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = sigterm_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    fprintf(stderr, "[supervisor] ready on %s\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = {1, 0};
        int rc = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) continue;

        int client = accept(ctx.server_fd, NULL, NULL);
        if (client < 0) continue;
        handle_request(&ctx, client);
        close(client);
    }

    fprintf(stderr, "[supervisor] shutting down\n");

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGKILL);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    while (waitpid(-1, NULL, 0) > 0);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);

    fprintf(stderr, "[supervisor] clean exit\n");
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    write(fd, req, sizeof(*req));

    control_response_t resp;
    if (read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp)) {
        printf("%s\n", resp.message);
        close(fd);
        return resp.status == 0 ? 0 : 1;
    }
    close(fd);
    return 1;
}

static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) { usage(argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    req.container_id[CONTAINER_ID_LEN-1] = '\0';
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    req.rootfs[PATH_MAX-1] = '\0';
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.command[CHILD_COMMAND_LEN-1] = '\0';
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) { usage(argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    req.container_id[CONTAINER_ID_LEN-1] = '\0';
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    req.rootfs[PATH_MAX-1] = '\0';
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.command[CHILD_COMMAND_LEN-1] = '\0';
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
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
    if (argc < 3) { usage(argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    req.container_id[CONTAINER_ID_LEN-1] = '\0';
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) { usage(argv[0]); return 1; }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    req.container_id[CONTAINER_ID_LEN-1] = '\0';
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
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
