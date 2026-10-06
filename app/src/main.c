/**
 * @file main.c
 * @brief Entry point for camera-server, the network security camera daemon.
 *
 * Threads:
 *   capture thread - owns the V4L2 device and is the only writer to the frame store
 *   client threads - one per HTTP connection, started by the HTTP server, read the frame store
 *   main thread    - runs the accept loop, handles SIGINT and SIGTERM, and stops and joins the
 *                    others on exit
 * Worker threads block SIGINT and SIGTERM, so signals always reach the main thread.
 * Signal and daemon handling are based on the aesdsocket assignment.
 */

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "frame_store.h"
#include "http_server.h"

#define DEFAULT_DEVICE              "/dev/video0"
#define DEFAULT_WIDTH               1280
#define DEFAULT_HEIGHT              720
#define DEFAULT_FPS                 30
#define DEFAULT_PORT                8080
#define DEFAULT_MAX_CLIENTS         4
#define MAX_CLIENTS_LIMIT           32

/* How often the capture and accept loops check whether to stop */
#define POLL_TIMEOUT_MS             500
/* No frame for this long means the camera has stalled */
#define FRAME_TIMEOUT_MS            5000
#define RATE_LOG_INTERVAL_SECONDS   10
/* Let auto exposure and autofocus settle (about 2 s at 30 fps) before saving a snapshot */
#define SNAPSHOT_SKIP_FRAMES        60

static volatile sig_atomic_t exit_requested = 0;

/* Set by the main thread to stop the capture thread */
static atomic_int capture_stop;
/* Set by the capture thread when it exits, so the main thread stops too */
static atomic_int capture_finished;

struct capture_thread_args {
    struct capture *cap;
    struct frame_store *store;
    int result;             /* 0 if stopped on request, -1 if the camera failed */
};

static void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;
}

static void setup_signals(void)
{
    struct sigaction sa;

    /* No SA_RESTART, so poll() returns with EINTR and the loop sees exit_requested */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* A browser closing the stream must not kill the server; send() errors are handled instead */
    signal(SIGPIPE, SIG_IGN);
}

static void daemon_mode(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    if (chdir("/") != 0) {
        perror("chdir");
    }
    if (freopen("/dev/null", "r", stdin) == NULL || freopen("/dev/null", "w", stdout) == NULL ||
        freopen("/dev/null", "w", stderr) == NULL) {
        exit(EXIT_FAILURE);
    }
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        syslog(LOG_ERR, "Cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fwrite(data, 1, len, f) != len) {
        syslog(LOG_ERR, "Write to %s failed", path);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        syslog(LOG_ERR, "Closing %s failed: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * Capture frames into the store until capture_stop is set.
 * @return 0 when stopped on request, -1 if the camera failed
 */
static int capture_loop(struct capture *cap, struct frame_store *store)
{
    double last_frame_time = monotonic_seconds();
    double rate_start = last_frame_time;
    unsigned long rate_frames = 0;

    while (!atomic_load(&capture_stop)) {
        struct pollfd pfd = { .fd = capture_fd(cap), .events = POLLIN };
        const void *data;
        size_t len;
        double now;
        int ret;

        ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "poll failed: %s", strerror(errno));
            return -1;
        }

        now = monotonic_seconds();
        if (ret == 0) {
            if ((now - last_frame_time) * 1000.0 >= FRAME_TIMEOUT_MS) {
                syslog(LOG_ERR, "No frame from the camera for %d ms", FRAME_TIMEOUT_MS);
                return -1;
            }
            continue;
        }

        /* POLLERR or POLLHUP (camera unplugged) also lands here; dequeue reports the error */
        ret = capture_dequeue(cap, &data, &len);
        if (ret == 1) {
            continue;
        }
        if (ret == -1) {
            syslog(LOG_ERR, "Camera stopped delivering frames");
            return -1;
        }

        if (frame_store_put(store, data, len) == -1) {
            syslog(LOG_ERR, "Out of memory storing a %zu byte frame", len);
            capture_release(cap);
            return -1;
        }
        if (capture_release(cap) == -1) {
            return -1;
        }
        last_frame_time = now;
        rate_frames++;

        if (now - rate_start >= RATE_LOG_INTERVAL_SECONDS) {
            syslog(LOG_INFO, "Capture rate %.1f fps, last frame %zu bytes",
                   (double)rate_frames / (now - rate_start), len);
            rate_start = now;
            rate_frames = 0;
        }
    }
    return 0;
}

static void *capture_thread(void *arg)
{
    struct capture_thread_args *args = arg;

    args->result = capture_loop(args->cap, args->store);
    /* No more frames are coming: wake every reader, and tell the main thread to stop */
    frame_store_shutdown(args->store);
    atomic_store(&capture_finished, 1);
    return NULL;
}

/** Start the capture thread with SIGINT and SIGTERM blocked, so only the main thread gets them. */
static int start_capture_thread(pthread_t *thread, struct capture_thread_args *args)
{
    sigset_t block;
    sigset_t old;
    int rc;

    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    rc = pthread_create(thread, NULL, capture_thread, args);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_create for capture failed: %s", strerror(rc));
        return -1;
    }
    return 0;
}

/** Accept loop: runs until a signal arrives or the capture thread exits. */
static void serve(struct http_server *srv)
{
    while (!exit_requested && !atomic_load(&capture_finished)) {
        struct pollfd pfd = { .fd = http_server_listen_fd(srv), .events = POLLIN };
        int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "poll failed: %s", strerror(errno));
            return;
        }
        if (ret > 0) {
            http_server_accept(srv);
        }
        http_server_reap(srv);
    }
}

/**
 * Wait until auto exposure has settled, then write the newest frame to path.
 * @return 0 on success or if interrupted by a signal, -1 on error
 */
static int save_snapshot(struct frame_store *store, const char *path)
{
    struct frame_copy frame = { 0 };
    int ret = 0;

    while (!exit_requested) {
        int wait = frame_store_wait_newer(store, SNAPSHOT_SKIP_FRAMES, &frame, POLL_TIMEOUT_MS);

        if (wait == 1) {
            continue;
        }
        if (wait == -1) {
            ret = -1;
        } else if (write_file(path, frame.data, frame.len) == -1) {
            ret = -1;
        } else {
            syslog(LOG_INFO, "Saved %zu byte snapshot to %s", frame.len, path);
        }
        break;
    }
    frame_copy_free(&frame);
    return ret;
}

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  -d, --daemon            run in the background\n"
           "  -D, --device PATH       video device (default %s)\n"
           "  -W, --width N           frame width (default %d)\n"
           "  -H, --height N          frame height (default %d)\n"
           "  -f, --fps N             frame rate (default %d)\n"
           "  -p, --port N            HTTP port (default %d)\n"
           "  -c, --max-clients N     maximum HTTP clients at once (default %d)\n"
           "  -s, --snapshot FILE     save one frame to FILE and exit\n"
           "  -v, --version           print the version and exit\n"
           "  -h, --help              print this help and exit\n",
           prog, DEFAULT_DEVICE, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_FPS, DEFAULT_PORT,
           DEFAULT_MAX_CLIENTS);
}

static int parse_positive(const char *arg, unsigned int *out)
{
    char *end;
    long value = strtol(arg, &end, 10);

    if (*arg == '\0' || *end != '\0' || value <= 0 || value > 10000) {
        return -1;
    }
    *out = (unsigned int)value;
    return 0;
}

int main(int argc, char *argv[])
{
    static const struct option long_options[] = {
        { "daemon",   no_argument,       NULL, 'd' },
        { "device",   required_argument, NULL, 'D' },
        { "width",    required_argument, NULL, 'W' },
        { "height",   required_argument, NULL, 'H' },
        { "fps",      required_argument, NULL, 'f' },
        { "port",     required_argument, NULL, 'p' },
        { "max-clients", required_argument, NULL, 'c' },
        { "snapshot", required_argument, NULL, 's' },
        { "version",  no_argument,       NULL, 'v' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    struct capture_config cfg = {
        .device = DEFAULT_DEVICE,
        .width = DEFAULT_WIDTH,
        .height = DEFAULT_HEIGHT,
        .fps = DEFAULT_FPS,
    };
    unsigned int port = DEFAULT_PORT;
    unsigned int max_clients = DEFAULT_MAX_CLIENTS;
    struct http_server *srv = NULL;
    struct capture_thread_args capture_args;
    pthread_t capture_tid;
    const char *snapshot_path = NULL;
    int run_as_daemon = 0;
    struct frame_store store;
    struct capture *cap;
    int opt;
    int ret;

    while ((opt = getopt_long(argc, argv, "dD:W:H:f:p:c:s:vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            run_as_daemon = 1;
            break;
        case 'D':
            cfg.device = optarg;
            break;
        case 'W':
        case 'H':
        case 'f': {
            unsigned int *target = opt == 'W' ? &cfg.width : opt == 'H' ? &cfg.height : &cfg.fps;

            if (parse_positive(optarg, target) == -1) {
                fprintf(stderr, "Invalid value for -%c: %s\n", opt, optarg);
                return EXIT_FAILURE;
            }
            break;
        }
        case 'p':
            if (parse_positive(optarg, &port) == -1 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'c':
            if (parse_positive(optarg, &max_clients) == -1 || max_clients > MAX_CLIENTS_LIMIT) {
                fprintf(stderr, "Invalid client limit (1 to %d): %s\n", MAX_CLIENTS_LIMIT, optarg);
                return EXIT_FAILURE;
            }
            break;
        case 's':
            snapshot_path = optarg;
            break;
        case 'v':
            printf("camera-server %s\n", VERSION);
            return EXIT_SUCCESS;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (run_as_daemon && snapshot_path == NULL) {
        daemon_mode();
    }
    /* Log to stderr as well when running in the foreground */
    openlog("camera-server", LOG_PID | (run_as_daemon ? 0 : LOG_PERROR), LOG_USER);
    setup_signals();
    syslog(LOG_INFO, "camera-server %s starting", VERSION);

    if (frame_store_init(&store) == -1) {
        closelog();
        return EXIT_FAILURE;
    }

    cap = capture_open(&cfg);
    if (cap == NULL) {
        frame_store_destroy(&store);
        closelog();
        return EXIT_FAILURE;
    }

    if (snapshot_path == NULL) {
        srv = http_server_open((unsigned short)port, max_clients, &store);
        if (srv == NULL) {
            capture_close(cap);
            frame_store_destroy(&store);
            closelog();
            return EXIT_FAILURE;
        }
    }

    capture_args.cap = cap;
    capture_args.store = &store;
    capture_args.result = 0;
    if (start_capture_thread(&capture_tid, &capture_args) == -1) {
        http_server_close(srv);
        capture_close(cap);
        frame_store_destroy(&store);
        closelog();
        return EXIT_FAILURE;
    }

    if (snapshot_path != NULL) {
        ret = save_snapshot(&store, snapshot_path);
    } else {
        serve(srv);
        ret = 0;
    }

    /* Stop the producer, wake all readers, then disconnect and join the clients */
    atomic_store(&capture_stop, 1);
    frame_store_shutdown(&store);
    http_server_close(srv);
    pthread_join(capture_tid, NULL);
    if (capture_args.result == -1) {
        ret = -1;
    }

    capture_close(cap);
    frame_store_destroy(&store);
    syslog(LOG_INFO, "camera-server stopped%s", ret == 0 ? "" : " after an error");
    closelog();
    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
