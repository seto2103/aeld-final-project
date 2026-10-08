/**
 * @file main.c
 * @brief Entry point for camera-server, the network security camera daemon.
 *
 * Threads:
 *   capture thread - owns the V4L2 device and is the only writer to the frame store
 *   client threads - one per HTTP connection, started by the HTTP server, read the frame store
 *   motion thread  - reads the frame store and reports when motion starts and ends
 *   recorder thread - reads every frame from the frame ring and saves a clip per motion event
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
#include "frame_ring.h"
#include "motion.h"
#include "recorder.h"
#include "thread_util.h"

#define DEFAULT_DEVICE              "/dev/video0"
#define DEFAULT_WIDTH               1280
#define DEFAULT_HEIGHT              720
#define DEFAULT_FPS                 30
#define DEFAULT_PORT                8080
#define DEFAULT_MAX_CLIENTS         4
#define DEFAULT_RECORD_DIR          "/data/recordings"
#define DEFAULT_RECORD_FREE_PERCENT 10
/* Seconds of video from before the motion at the start of each clip */
#define PRE_EVENT_SECONDS           5
/* Extra seconds the frame ring holds, so a slow SD card write doesn't lose frames */
#define RING_SLACK_SECONDS          5
#define MAX_CLIENTS_LIMIT           32

/* getopt_long values for options that only have a long form */
enum {
    OPT_MOTION_PIXEL = 256,
    OPT_MOTION_PERCENT,
    OPT_MOTION_HOLDOFF,
    OPT_RECORD_DIR,
    OPT_RECORD_FREE,
};

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
    struct frame_ring *ring;        /* NULL in snapshot mode */
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
 * Capture frames into the store, and the ring if there is one, until capture_stop is set.
 * @return 0 when stopped on request, -1 if the camera failed
 */
static int capture_loop(struct capture *cap, struct frame_store *store, struct frame_ring *ring)
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

        if (frame_store_put(store, data, len) == -1 ||
            (ring != NULL && frame_ring_push(ring, data, len) == -1)) {
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

    args->result = capture_loop(args->cap, args->store, args->ring);
    /* No more frames are coming: wake every reader, and tell the main thread to stop */
    frame_store_shutdown(args->store);
    if (args->ring != NULL) {
        frame_ring_shutdown(args->ring);
    }
    atomic_store(&capture_finished, 1);
    return NULL;
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
           "      --motion-percent P  percentage of pixels that must change for motion (default %.1f)\n"
           "      --motion-pixel N    brightness change (1-255) that counts a pixel as changed\n"
           "                          (default %d)\n"
           "      --motion-holdoff MS motion ends after this long without motion (default %d)\n"
           "      --record-dir DIR    save motion clips in DIR, \"\" to disable (default %s)\n"
           "      --record-free P     delete the oldest clips to keep P%% of the space free\n"
           "                          (default %d)\n"
           "  -s, --snapshot FILE     save one frame to FILE and exit\n"
           "  -v, --version           print the version and exit\n"
           "  -h, --help              print this help and exit\n",
           prog, DEFAULT_DEVICE, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_FPS, DEFAULT_PORT,
           DEFAULT_MAX_CLIENTS, MOTION_DEFAULT_TRIGGER_PERCENT, MOTION_DEFAULT_PIXEL_THRESHOLD,
           MOTION_DEFAULT_HOLDOFF_MS, DEFAULT_RECORD_DIR, DEFAULT_RECORD_FREE_PERCENT);
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
        { "motion-percent", required_argument, NULL, OPT_MOTION_PERCENT },
        { "motion-pixel", required_argument, NULL, OPT_MOTION_PIXEL },
        { "motion-holdoff", required_argument, NULL, OPT_MOTION_HOLDOFF },
        { "record-dir", required_argument, NULL, OPT_RECORD_DIR },
        { "record-free", required_argument, NULL, OPT_RECORD_FREE },
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
    struct motion_config motion_cfg = {
        .pixel_threshold = MOTION_DEFAULT_PIXEL_THRESHOLD,
        .trigger_percent = MOTION_DEFAULT_TRIGGER_PERCENT,
        .holdoff_ms = MOTION_DEFAULT_HOLDOFF_MS,
    };
    struct motion *motion = NULL;
    const char *record_dir = DEFAULT_RECORD_DIR;
    unsigned int record_free = DEFAULT_RECORD_FREE_PERCENT;
    struct recorder *recorder = NULL;
    struct frame_ring ring;
    int have_ring = 0;
    struct capture_thread_args capture_args;
    pthread_t capture_tid;
    const char *snapshot_path = NULL;
    int run_as_daemon = 0;
    struct frame_store store;
    struct capture *cap;
    int opt;
    int ret;
    int rc;

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
        case OPT_MOTION_PERCENT: {
            char *end;

            motion_cfg.trigger_percent = strtod(optarg, &end);
            if (*optarg == '\0' || *end != '\0' || motion_cfg.trigger_percent <= 0 ||
                motion_cfg.trigger_percent > 100) {
                fprintf(stderr, "Invalid motion percentage (above 0, up to 100): %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        }
        case OPT_MOTION_PIXEL:
            if (parse_positive(optarg, &motion_cfg.pixel_threshold) == -1 ||
                motion_cfg.pixel_threshold > 255) {
                fprintf(stderr, "Invalid motion pixel threshold (1 to 255): %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case OPT_MOTION_HOLDOFF:
            if (parse_positive(optarg, &motion_cfg.holdoff_ms) == -1) {
                fprintf(stderr, "Invalid motion hold-off (1 to 10000 ms): %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case OPT_RECORD_DIR:
            record_dir = optarg;
            break;
        case OPT_RECORD_FREE:
            if (parse_positive(optarg, &record_free) == -1 || record_free > 95) {
                fprintf(stderr, "Invalid free space percentage (1 to 95): %s\n", optarg);
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

    ret = -1;
    if (frame_store_init(&store) == -1) {
        closelog();
        return EXIT_FAILURE;
    }

    cap = capture_open(&cfg);
    if (cap == NULL) {
        goto out_store;
    }

    if (snapshot_path == NULL) {
        unsigned int width, height, fps;

        capture_get_format(cap, &width, &height, &fps);
        if (*record_dir != '\0') {
            if (frame_ring_init(&ring, (PRE_EVENT_SECONDS + RING_SLACK_SECONDS) * fps) == -1) {
                goto out_capture;
            }
            have_ring = 1;
        }
        srv = http_server_open((unsigned short)port, max_clients, &store);
        if (srv == NULL) {
            goto out_capture;
        }
    }

    capture_args.cap = cap;
    capture_args.store = &store;
    capture_args.ring = have_ring ? &ring : NULL;
    capture_args.result = 0;
    rc = thread_create_signals_blocked(&capture_tid, capture_thread, &capture_args);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_create for capture failed: %s", strerror(rc));
        goto out_server;
    }

    if (snapshot_path != NULL) {
        ret = save_snapshot(&store, snapshot_path);
    } else {
        motion = motion_start(&motion_cfg, &store);
        if (motion != NULL && have_ring) {
            struct recorder_config rec_cfg = { .dir = record_dir };

            capture_get_format(cap, &rec_cfg.width, &rec_cfg.height, &rec_cfg.fps);
            rec_cfg.pre_event_frames = PRE_EVENT_SECONDS * rec_cfg.fps;
            rec_cfg.free_percent = record_free;
            recorder = recorder_start(&rec_cfg, &ring, motion);
        } else if (motion != NULL) {
            syslog(LOG_INFO, "Recording disabled by --record-dir \"\"");
        }
        if (motion != NULL && (recorder != NULL || !have_ring)) {
            serve(srv);
            ret = 0;
        }
    }

    /* Stop the producer, wake all readers, then disconnect and join the clients and consumers */
    atomic_store(&capture_stop, 1);
    frame_store_shutdown(&store);
    if (have_ring) {
        frame_ring_shutdown(&ring);
    }
    http_server_close(srv);
    srv = NULL;
    recorder_stop(recorder);
    motion_stop(motion);
    pthread_join(capture_tid, NULL);
    if (capture_args.result == -1) {
        ret = -1;
    }

out_server:
    http_server_close(srv);
out_capture:
    if (have_ring) {
        frame_ring_destroy(&ring);
    }
    capture_close(cap);
out_store:
    frame_store_destroy(&store);
    syslog(LOG_INFO, "camera-server stopped%s", ret == 0 ? "" : " after an error");
    closelog();
    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
