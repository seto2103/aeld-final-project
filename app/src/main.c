/**
 * @file main.c
 * @brief Entry point for camera-server, the network security camera daemon.
 *
 * Single-threaded version: one poll() loop dequeues MJPEG frames from the camera, keeps the
 * newest one in the frame store and logs the capture rate. Signal and daemon handling are based
 * on the aesdsocket assignment.
 */

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "frame_store.h"

#define DEFAULT_DEVICE              "/dev/video0"
#define DEFAULT_WIDTH               1280
#define DEFAULT_HEIGHT              720
#define DEFAULT_FPS                 30

#define POLL_TIMEOUT_MS             1000
/* No frame for this long means the camera has stalled */
#define FRAME_TIMEOUT_MS            5000
#define RATE_LOG_INTERVAL_SECONDS   10
/* Let auto exposure and autofocus settle (about 2 s at 30 fps) before saving a snapshot */
#define SNAPSHOT_SKIP_FRAMES        60

static volatile sig_atomic_t exit_requested = 0;

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
 * Capture until a signal arrives, or, in snapshot mode, until one frame has been saved.
 * @return 0 on a clean exit, -1 if the camera failed
 */
static int run(struct capture *cap, struct frame_store *store, const char *snapshot_path)
{
    struct pollfd pfd = { .fd = capture_fd(cap), .events = POLLIN };
    double last_frame_time = monotonic_seconds();
    double rate_start = last_frame_time;
    unsigned long rate_frames = 0;

    while (!exit_requested) {
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
            syslog(LOG_ERR, "Camera stopped delivering frames, exiting");
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

        if (snapshot_path != NULL && store->seq > SNAPSHOT_SKIP_FRAMES) {
            if (write_file(snapshot_path, store->data, store->len) == -1) {
                return -1;
            }
            syslog(LOG_INFO, "Saved %zu byte snapshot to %s", store->len, snapshot_path);
            return 0;
        }

        if (now - rate_start >= RATE_LOG_INTERVAL_SECONDS) {
            syslog(LOG_INFO, "Capture rate %.1f fps, last frame %zu bytes",
                   (double)rate_frames / (now - rate_start), len);
            rate_start = now;
            rate_frames = 0;
        }
    }
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  -d, --daemon            run in the background\n"
           "  -D, --device PATH       video device (default %s)\n"
           "  -W, --width N           frame width (default %d)\n"
           "  -H, --height N          frame height (default %d)\n"
           "  -f, --fps N             frame rate (default %d)\n"
           "  -s, --snapshot FILE     save one frame to FILE and exit\n"
           "  -v, --version           print the version and exit\n"
           "  -h, --help              print this help and exit\n",
           prog, DEFAULT_DEVICE, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_FPS);
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
    const char *snapshot_path = NULL;
    int run_as_daemon = 0;
    struct frame_store store;
    struct capture *cap;
    int opt;
    int ret;

    while ((opt = getopt_long(argc, argv, "dD:W:H:f:s:vh", long_options, NULL)) != -1) {
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

    cap = capture_open(&cfg);
    if (cap == NULL) {
        closelog();
        return EXIT_FAILURE;
    }

    frame_store_init(&store);
    ret = run(cap, &store, snapshot_path);

    capture_close(cap);
    frame_store_free(&store);
    syslog(LOG_INFO, "camera-server stopped%s", ret == 0 ? "" : " after an error");
    closelog();
    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
