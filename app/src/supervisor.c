/**
 * @file supervisor.c
 * @brief camera-supervisor: runs camera-server as a child process, starts it again whenever it
 *        exits, and feeds the hardware watchdog so the board reboots if the whole system hangs.
 *
 * Single threaded. SIGCHLD, SIGINT and SIGTERM stay blocked and are collected with
 * sigtimedwait(), whose timeout also paces the watchdog feeds, restart delays and the stop
 * deadline.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SERVER              "/usr/bin/camera-server"
#define DEFAULT_WATCHDOG            "/dev/watchdog"
/* 15 s is also the longest timeout the Pi's bcm2835_wdt supports */
#define DEFAULT_WATCHDOG_TIMEOUT    15
#define MAX_WATCHDOG_TIMEOUT        60
#define MIN_RESTART_DELAY           1
#define MAX_RESTART_DELAY           30
/* A camera-server that ran this long was healthy, so the restart delay goes back to the minimum */
#define STABLE_SECONDS              60
/* How long camera-server gets to exit after SIGTERM before it is killed */
#define STOP_TIMEOUT_SECONDS        10
/* Longest sleep when there is nothing to wait for */
#define IDLE_SECONDS                60

struct supervisor {
    char **server_argv;
    pid_t child;                /* 0 while camera-server is not running */
    double child_start;
    double restart_at;
    unsigned int restart_delay;
    int stopping;
    double stop_deadline;
    int killed;
    int wdt_fd;                 /* -1 without a watchdog */
    double feed_interval;
    double next_feed;
};

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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

    /* SIGCHLD keeps its default action, unlike in camera-server, so waitpid() still works */
    signal(SIGHUP, SIG_IGN);

    if (chdir("/") != 0) {
        perror("chdir");
    }
    /* camera-server inherits these, and logs to syslog */
    if (freopen("/dev/null", "r", stdin) == NULL || freopen("/dev/null", "w", stdout) == NULL ||
        freopen("/dev/null", "w", stderr) == NULL) {
        exit(EXIT_FAILURE);
    }
}

/**
 * Open and arm the watchdog. From here on it must be fed, or the board reboots.
 * @return 0 on success, -1 if running without a watchdog (logged)
 */
static int watchdog_open(struct supervisor *s, const char *path, int timeout)
{
    if (*path == '\0') {
        syslog(LOG_INFO, "Hardware watchdog disabled");
        return -1;
    }

    /* O_CLOEXEC: camera-server must not hold the watchdog open after the supervisor exits */
    s->wdt_fd = open(path, O_WRONLY | O_CLOEXEC);
    if (s->wdt_fd == -1) {
        syslog(LOG_WARNING, "Cannot open %s: %s; running without the hardware watchdog", path,
               strerror(errno));
        return -1;
    }

    if (ioctl(s->wdt_fd, WDIOC_SETTIMEOUT, &timeout) == -1) {
        syslog(LOG_WARNING, "Cannot set the watchdog timeout to %d s: %s", timeout,
               strerror(errno));
    }
    if (ioctl(s->wdt_fd, WDIOC_GETTIMEOUT, &timeout) == -1 || timeout < 1) {
        timeout = DEFAULT_WATCHDOG_TIMEOUT;
    }
    /* Three feeds per timeout, so one late wakeup doesn't reboot the board */
    s->feed_interval = timeout >= 3 ? timeout / 3.0 : 1.0;
    s->next_feed = 0;
    syslog(LOG_INFO, "Watchdog %s armed with a %d s timeout", path, timeout);
    return 0;
}

static void watchdog_feed(struct supervisor *s, double now)
{
    if (s->wdt_fd == -1 || now < s->next_feed) {
        return;
    }
    if (ioctl(s->wdt_fd, WDIOC_KEEPALIVE, 0) == -1) {
        syslog(LOG_ERR, "Feeding the watchdog failed: %s", strerror(errno));
    }
    s->next_feed = now + s->feed_interval;
}

/** Disarm the watchdog with the magic close, so a clean stop doesn't reboot the board. */
static void watchdog_close(struct supervisor *s)
{
    if (s->wdt_fd == -1) {
        return;
    }
    if (write(s->wdt_fd, "V", 1) != 1) {
        syslog(LOG_ERR, "Watchdog magic close failed: %s", strerror(errno));
    }
    close(s->wdt_fd);
    s->wdt_fd = -1;
    syslog(LOG_INFO, "Watchdog disarmed");
}

static void start_server(struct supervisor *s, double now)
{
    pid_t pid = fork();

    if (pid == -1) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        sigset_t none;

        /* The blocked signals are inherited across exec, and camera-server needs SIGTERM */
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execv(s->server_argv[0], s->server_argv);
        syslog(LOG_ERR, "Cannot run %s: %s", s->server_argv[0], strerror(errno));
        _exit(127);
    }

    s->child = pid;
    s->child_start = now;
    syslog(LOG_INFO, "Started %s (pid %d)", s->server_argv[0], (int)pid);
}

static void schedule_restart(struct supervisor *s, double now, double ran)
{
    if (ran >= STABLE_SECONDS) {
        s->restart_delay = MIN_RESTART_DELAY;
    }
    s->restart_at = now + s->restart_delay;
    syslog(LOG_INFO, "Restarting camera-server in %u s", s->restart_delay);

    s->restart_delay *= 2;
    if (s->restart_delay > MAX_RESTART_DELAY) {
        s->restart_delay = MAX_RESTART_DELAY;
    }
}

static void log_exit(pid_t pid, int status, double ran)
{
    if (WIFEXITED(status)) {
        syslog(WEXITSTATUS(status) == 0 ? LOG_INFO : LOG_WARNING,
               "camera-server (pid %d) exited with status %d after %.1f s", (int)pid,
               WEXITSTATUS(status), ran);
    } else if (WIFSIGNALED(status)) {
        syslog(LOG_WARNING, "camera-server (pid %d) was killed by signal %d (%s) after %.1f s",
               (int)pid, WTERMSIG(status), strsignal(WTERMSIG(status)), ran);
    }
}

static void reap_children(struct supervisor *s, double now)
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        double ran;

        if (pid != s->child) {
            continue;
        }
        ran = now - s->child_start;
        log_exit(pid, status, ran);
        s->child = 0;
        if (!s->stopping) {
            schedule_restart(s, now, ran);
        }
    }
}

static void begin_stop(struct supervisor *s, int signo, double now)
{
    if (s->stopping) {
        return;
    }
    syslog(LOG_INFO, "Caught %s, stopping", strsignal(signo));
    s->stopping = 1;
    s->stop_deadline = now + STOP_TIMEOUT_SECONDS;
    if (s->child != 0) {
        kill(s->child, SIGTERM);
    }
}

/** Seconds until the next thing the loop has to do, or 0 if it is already due. */
static double seconds_to_wake(const struct supervisor *s, double now)
{
    double wake = now + IDLE_SECONDS;

    if (s->wdt_fd != -1 && s->next_feed < wake) {
        wake = s->next_feed;
    }
    if (s->child == 0 && !s->stopping && s->restart_at < wake) {
        wake = s->restart_at;
    }
    if (s->stopping && !s->killed && s->stop_deadline < wake) {
        wake = s->stop_deadline;
    }
    return wake > now ? wake - now : 0;
}

/** Run until SIGINT or SIGTERM, then stop camera-server. @return 0, or -1 on error */
static int run(struct supervisor *s, const sigset_t *sigs)
{
    for (;;) {
        double now = monotonic_seconds();
        double wait;
        struct timespec ts;
        int sig;

        watchdog_feed(s, now);

        if (s->child == 0) {
            if (s->stopping) {
                return 0;
            }
            if (now >= s->restart_at) {
                start_server(s, now);
                if (s->child == 0) {
                    schedule_restart(s, now, 0);
                }
            }
        } else if (s->stopping && !s->killed && now >= s->stop_deadline) {
            syslog(LOG_WARNING, "camera-server did not stop within %d s, killing it",
                   STOP_TIMEOUT_SECONDS);
            kill(s->child, SIGKILL);
            s->killed = 1;
        }

        wait = seconds_to_wake(s, now);
        ts.tv_sec = (time_t)wait;
        ts.tv_nsec = (long)((wait - (double)ts.tv_sec) * 1e9);

        sig = sigtimedwait(sigs, NULL, &ts);
        now = monotonic_seconds();
        if (sig == -1) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "sigtimedwait failed: %s", strerror(errno));
            return -1;
        }
        if (sig == SIGCHLD) {
            reap_children(s, now);
        } else {
            begin_stop(s, sig, now);
        }
    }
}

static int parse_timeout(const char *arg, int *out)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(arg, &end, 10);
    if (*arg == '\0' || *end != '\0' || errno != 0 || value < 1 || value > MAX_WATCHDOG_TIMEOUT) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s [options] [-- camera-server options]\n"
           "Runs camera-server, restarts it after it exits and feeds the hardware watchdog.\n"
           "\n"
           "  -d, --daemon            run in the background\n"
           "  -s, --server PATH       camera-server binary (default %s)\n"
           "  -w, --watchdog PATH     watchdog device (default %s; \"\" disables)\n"
           "  -t, --timeout N         watchdog timeout in seconds (default %d)\n"
           "  -v, --version           print the version and exit\n"
           "  -h, --help              show this help\n",
           prog, DEFAULT_SERVER, DEFAULT_WATCHDOG, DEFAULT_WATCHDOG_TIMEOUT);
}

int main(int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "daemon",   no_argument,       NULL, 'd' },
        { "server",   required_argument, NULL, 's' },
        { "watchdog", required_argument, NULL, 'w' },
        { "timeout",  required_argument, NULL, 't' },
        { "version",  no_argument,       NULL, 'v' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    struct supervisor s = {
        .restart_delay = MIN_RESTART_DELAY,
        .wdt_fd = -1,
    };
    const char *server = DEFAULT_SERVER;
    const char *watchdog = DEFAULT_WATCHDOG;
    int timeout = DEFAULT_WATCHDOG_TIMEOUT;
    int run_as_daemon = 0;
    sigset_t sigs;
    int nargs;
    int opt;
    int ret;

    /* "+" stops at the first argument that isn't an option, the start of camera-server's own */
    while ((opt = getopt_long(argc, argv, "+ds:w:t:vh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd':
            run_as_daemon = 1;
            break;
        case 's':
            server = optarg;
            break;
        case 'w':
            watchdog = optarg;
            break;
        case 't':
            if (parse_timeout(optarg, &timeout) == -1) {
                fprintf(stderr, "Invalid watchdog timeout (1 to %d s): %s\n",
                        MAX_WATCHDOG_TIMEOUT, optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'v':
            printf("camera-supervisor %s\n", VERSION);
            return EXIT_SUCCESS;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* camera-server gets the remaining arguments, after its own path */
    nargs = argc - optind;
    s.server_argv = calloc((size_t)nargs + 2, sizeof(char *));
    if (s.server_argv == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }
    s.server_argv[0] = (char *)server;
    memcpy(&s.server_argv[1], &argv[optind], (size_t)nargs * sizeof(char *));

    if (run_as_daemon) {
        daemon_mode();
    }
    openlog("camera-supervisor", LOG_PID | (run_as_daemon ? 0 : LOG_PERROR), LOG_USER);
    syslog(LOG_INFO, "camera-supervisor %s starting", VERSION);

    sigemptyset(&sigs);
    sigaddset(&sigs, SIGCHLD);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigprocmask(SIG_BLOCK, &sigs, NULL);

    watchdog_open(&s, watchdog, timeout);
    ret = run(&s, &sigs);
    watchdog_close(&s);

    syslog(LOG_INFO, "camera-supervisor stopped");
    closelog();
    free(s.server_argv);
    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
