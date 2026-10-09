/**
 * @file display.c
 * @brief Display thread: shows the newest camera frame and a status bar on a 16-bit framebuffer,
 *        such as the 3.5" SPI display (fb_ili9486, 480x320).
 *
 * About 10 frames per second are decoded with libjpeg-turbo DCT scaling straight to RGB565, at
 * the largest scale (in eighths) that fits the screen and leaves room for the status bar:
 * 1280x720 becomes 480x270 at 3/8. Each frame is decoded into a back buffer and copied to the
 * memory-mapped framebuffer; the fbtft driver then sends the changed pages over SPI, which
 * limits the screen to about 10 fps. The status bar is redrawn only when its text changes, so
 * its pages are only sent then. The clip count and free space it shows are read every
 * STORAGE_INTERVAL_SECONDS, since that reads the recordings directory.
 */

#include "display.h"
#include "thread_util.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>

/* Show about 10 frames per second, about as fast as the SPI display can be updated */
#define DISPLAY_INTERVAL_MS     100
/* How long to wait for a frame before checking whether to stop */
#define FRAME_WAIT_MS           500
/* Rows kept below the image for the status bar: two lines of scale 2 text */
#define MIN_BAR_HEIGHT          44
#define STATS_LOG_INTERVAL_SECONDS  10
#define STORAGE_INTERVAL_SECONDS    5
/* Time for the screen update started by fsync() to begin, see fb_close() */
#define CLEAR_START_SECONDS         0.05

/* RGB565 colors */
#define COLOR_BLACK             0x0000
#define COLOR_WHITE             0xffff
#define COLOR_RED               0xf800
#define COLOR_BAR               0x2104      /* dark gray */

/* Built-in 5x7 font, only the characters the status bar uses */
#define GLYPH_WIDTH             5
#define GLYPH_HEIGHT            7

struct glyph {
    char c;
    const char *rows[GLYPH_HEIGHT];
};

static const struct glyph font[] = {
    { '0', { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." } },
    { '1', { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." } },
    { '2', { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" } },
    { '3', { "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###." } },
    { '4', { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." } },
    { '5', { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." } },
    { '6', { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." } },
    { '7', { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." } },
    { '8', { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." } },
    { '9', { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." } },
    { '-', { ".....", ".....", ".....", "#####", ".....", ".....", "....." } },
    { ':', { ".....", ".##..", ".##..", ".....", ".##..", ".##..", "....." } },
    { '.', { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." } },
    { 'C', { ".###.", "#...#", "#....", "#....", "#....", "#...#", ".###." } },
    { 'E', { "#####", "#....", "#....", "####.", "#....", "#....", "#####" } },
    { 'F', { "#####", "#....", "#....", "####.", "#....", "#....", "#...." } },
    { 'G', { ".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####" } },
    { 'I', { ".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###." } },
    { 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
    { 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#" } },
    { 'N', { "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#" } },
    { 'P', { "####.", "#...#", "#...#", "####.", "#....", "#....", "#...." } },
    { 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
    { 'S', { ".####", "#....", "#....", ".###.", "....#", "....#", "####." } },
    { 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
    { 'U', { "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
};

struct display {
    struct frame_store *store;
    const struct recorder *recorder;
    const struct http_server *srv;
    pthread_t thread;
    atomic_int stop;
    int fd;
    uint16_t *fb;                   /* memory-mapped framebuffer */
    size_t fb_size;
    unsigned int xres;
    unsigned int yres;
    unsigned int stride;            /* framebuffer pixels per line */
    uint16_t *back;                 /* back buffer, xres x yres */
    unsigned int scale_num;         /* image is scale_num/8 of the camera frame */
    unsigned int img_x;             /* image position and size */
    unsigned int img_w;
    unsigned int img_h;
    char storage[32];               /* clip count and free space, for the status bar */
    double storage_due;             /* when to read them again */
    char status[128];               /* status bar as last drawn */
};

/* libjpeg calls error_exit() on a fatal error and by default exits the process */
struct jpeg_error_jump {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_error_jump *err = (struct jpeg_error_jump *)cinfo->err;

    longjmp(err->jump, 1);
}

/* Silence libjpeg warnings such as "Corrupt JPEG data"; a bad frame is just skipped */
static void jpeg_emit_message(j_common_ptr cinfo, int msg_level)
{
    (void)cinfo;
    (void)msg_level;
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_seconds(double seconds)
{
    struct timespec ts;

    if (seconds <= 0) {
        return;
    }
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

static void fill_rect(struct display *d, unsigned int x, unsigned int y, unsigned int w,
                      unsigned int h, uint16_t color)
{
    unsigned int row;
    unsigned int col;

    for (row = y; row < y + h && row < d->yres; row++) {
        for (col = x; col < x + w && col < d->xres; col++) {
            d->back[(size_t)row * d->xres + col] = color;
        }
    }
}

static void fill_circle(struct display *d, int cx, int cy, int r, uint16_t color)
{
    int x;
    int y;

    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r && cx + x >= 0 && cy + y >= 0) {
                fill_rect(d, (unsigned int)(cx + x), (unsigned int)(cy + y), 1, 1, color);
            }
        }
    }
}

static const struct glyph *find_glyph(char c)
{
    size_t i;

    for (i = 0; i < sizeof(font) / sizeof(font[0]); i++) {
        if (font[i].c == c) {
            return &font[i];
        }
    }
    return NULL;                    /* drawn as a space */
}

/** Width in pixels of text drawn at the given scale. */
static unsigned int text_width(const char *text, unsigned int scale)
{
    size_t len = strlen(text);

    return len == 0 ? 0 : (unsigned int)(len * (GLYPH_WIDTH + 1) - 1) * scale;
}

static void draw_text(struct display *d, unsigned int x, unsigned int y, unsigned int scale,
                      uint16_t color, const char *text)
{
    for (; *text != '\0'; text++, x += (GLYPH_WIDTH + 1) * scale) {
        const struct glyph *g = find_glyph(*text);
        unsigned int row;
        unsigned int col;

        if (g == NULL) {
            continue;
        }
        for (row = 0; row < GLYPH_HEIGHT; row++) {
            for (col = 0; col < GLYPH_WIDTH; col++) {
                if (g->rows[row][col] == '#') {
                    fill_rect(d, x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

/** Copy rows of the back buffer to the framebuffer. */
static void flush_rows(struct display *d, unsigned int x, unsigned int y, unsigned int w,
                       unsigned int h)
{
    unsigned int row;

    for (row = y; row < y + h; row++) {
        memcpy(d->fb + (size_t)row * d->stride + x, d->back + (size_t)row * d->xres + x,
               (size_t)w * sizeof(uint16_t));
    }
}

/** Every few seconds, read the clip count and free space for the status bar. */
static void update_storage(struct display *d)
{
    double now = monotonic_seconds();
    unsigned long long free_bytes;
    unsigned int clips;

    if (now < d->storage_due) {
        return;
    }
    d->storage_due = now + STORAGE_INTERVAL_SECONDS;

    if (recorder_storage(d->recorder, &clips, &free_bytes) == -1) {
        d->storage[0] = '\0';
    } else if (free_bytes >= 1ull << 30) {
        /* Binary units, like df -h */
        snprintf(d->storage, sizeof(d->storage), "  CLIPS %u  FREE %.1fG", clips,
                 (double)free_bytes / (double)(1ull << 30));
    } else {
        snprintf(d->storage, sizeof(d->storage), "  CLIPS %u  FREE %lluM", clips,
                 free_bytes >> 20);
    }
}

/** Redraw the status bar if its contents changed since it was last drawn. */
static void update_status(struct display *d)
{
    unsigned int bar_y = d->img_h;
    unsigned int bar_h = d->yres - d->img_h;
    int recording = recorder_recording(d->recorder);
    unsigned int clients = d->srv != NULL ? http_server_clients(d->srv) : 0;
    char clock_line[32];
    char clients_line[64];
    char status[sizeof(d->status)];
    time_t now = time(NULL);
    struct tm tm;

    gmtime_r(&now, &tm);
    strftime(clock_line, sizeof(clock_line), "%Y-%m-%d %H:%M:%S UTC", &tm);
    update_storage(d);
    snprintf(clients_line, sizeof(clients_line), "CLIENTS %u%s", clients, d->storage);
    snprintf(status, sizeof(status), "%s|%s|%d", clock_line, clients_line, recording);
    if (strcmp(status, d->status) == 0) {
        return;
    }
    memcpy(d->status, status, sizeof(status));

    fill_rect(d, 0, bar_y, d->xres, bar_h, COLOR_BAR);
    draw_text(d, 8, bar_y + (bar_h - 4 * GLYPH_HEIGHT - 6) / 2, 2, COLOR_WHITE, clock_line);
    draw_text(d, 8, bar_y + (bar_h - 4 * GLYPH_HEIGHT - 6) / 2 + 2 * GLYPH_HEIGHT + 6, 2,
              COLOR_WHITE, clients_line);
    if (recording) {
        unsigned int text_x = d->xres - 8 - text_width("REC", 3);
        unsigned int text_y = bar_y + (bar_h - 3 * GLYPH_HEIGHT) / 2;

        fill_circle(d, (int)text_x - 14, (int)(bar_y + bar_h / 2), 8, COLOR_RED);
        draw_text(d, text_x, text_y, 3, COLOR_RED, "REC");
    }
    flush_rows(d, 0, bar_y, d->xres, bar_h);
}

/**
 * Decode a JPEG frame into the image area of the back buffer and copy it to the screen.
 * @return 0 on success, -1 if the frame could not be decoded
 */
static int show_frame(struct display *d, const unsigned char *data, size_t len)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_jump jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    jerr.pub.emit_message = jpeg_emit_message;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.out_color_space = JCS_RGB565;
    /* RGB565 has only 32 to 64 levels per color; dithering hides the banding this causes */
    cinfo.dither_mode = JDITHER_ORDERED;
    cinfo.scale_num = d->scale_num;
    cinfo.scale_denom = 8;
    cinfo.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&cinfo);

    /* A frame of another size than the one the layout was made for */
    if (cinfo.output_width != d->img_w || cinfo.output_height != d->img_h) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = (JSAMPROW)(d->back + (size_t)cinfo.output_scanline * d->xres + d->img_x);

        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    flush_rows(d, d->img_x, 0, d->img_w, d->img_h);
    return 0;
}

static void *display_thread(void *arg)
{
    struct display *d = arg;
    struct frame_copy frame = { 0 };
    uint64_t last_seq = 0;
    double next_due = monotonic_seconds();
    /* For the periodic log of the display rate */
    double log_start = next_due;
    double log_decode_seconds = 0;
    unsigned long log_frames = 0;
    unsigned long decode_errors = 0;

    while (!atomic_load(&d->stop)) {
        double start;
        double now;
        int ret;

        sleep_seconds(next_due - monotonic_seconds());
        ret = frame_store_wait_newer(d->store, last_seq, &frame, FRAME_WAIT_MS);
        if (ret == 1) {
            update_status(d);
            continue;
        }
        if (ret == -1) {
            break;
        }
        last_seq = frame.seq;

        start = monotonic_seconds();
        next_due = start + DISPLAY_INTERVAL_MS / 1000.0;
        if (show_frame(d, frame.data, frame.len) == -1) {
            decode_errors++;
        } else {
            log_frames++;
        }
        update_status(d);
        now = monotonic_seconds();
        log_decode_seconds += now - start;

        if (now - log_start >= STATS_LOG_INTERVAL_SECONDS) {
            syslog(LOG_DEBUG, "Display %.1f fps, %.1f ms per frame, %lu decode errors",
                   (double)log_frames / (now - log_start),
                   log_frames ? log_decode_seconds * 1000.0 / log_frames : 0.0, decode_errors);
            log_start = now;
            log_decode_seconds = 0;
            log_frames = 0;
            decode_errors = 0;
        }
    }

    frame_copy_free(&frame);
    return NULL;
}

/**
 * Choose the largest scale, in eighths, at which the camera frame fits the screen with room
 * left for the status bar.
 * @return 0 on success, -1 if even 1/8 doesn't fit
 */
static int choose_layout(struct display *d, unsigned int width, unsigned int height)
{
    unsigned int num;

    if (d->yres <= MIN_BAR_HEIGHT) {
        return -1;
    }
    for (num = 8; num >= 1; num--) {
        /* libjpeg rounds scaled sizes up */
        unsigned int w = (width * num + 7) / 8;
        unsigned int h = (height * num + 7) / 8;

        if (w <= d->xres && h <= d->yres - MIN_BAR_HEIGHT) {
            d->scale_num = num;
            d->img_w = w;
            d->img_h = h;
            d->img_x = (d->xres - w) / 2;
            return 0;
        }
    }
    return -1;
}

/** Open and map the framebuffer. @return 0 on success, -1 if it can't be used (logged) */
static int fb_open(struct display *d, const char *device)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    d->fd = open(device, O_RDWR | O_CLOEXEC);
    if (d->fd == -1) {
        syslog(LOG_WARNING, "Cannot open %s: %s; running without the display", device,
               strerror(errno));
        return -1;
    }
    if (ioctl(d->fd, FBIOGET_VSCREENINFO, &var) == -1 ||
        ioctl(d->fd, FBIOGET_FSCREENINFO, &fix) == -1) {
        syslog(LOG_WARNING, "%s is not a framebuffer: %s; running without the display", device,
               strerror(errno));
        goto err_close;
    }
    if (var.bits_per_pixel != 16) {
        syslog(LOG_WARNING, "%s has %u bits per pixel, only 16 is supported; running without "
               "the display", device, var.bits_per_pixel);
        goto err_close;
    }

    d->xres = var.xres;
    d->yres = var.yres;
    d->stride = fix.line_length / sizeof(uint16_t);
    d->fb_size = (size_t)fix.line_length * var.yres;
    d->fb = mmap(NULL, d->fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->fb == MAP_FAILED) {
        syslog(LOG_WARNING, "Cannot map %s: %s; running without the display", device,
               strerror(errno));
        goto err_close;
    }
    return 0;

err_close:
    close(d->fd);
    return -1;
}

/** Clear the screen, and make sure the clear reaches it before the framebuffer is closed. */
static void fb_close(struct display *d)
{
    memset(d->fb, 0, d->fb_size);
    /* The driver sends changed pages to the display a little later, and closing the
     * framebuffer cancels an update that hasn't started yet. fsync() starts it at once, and
     * once it is running, closing waits for it to finish. */
    fsync(d->fd);
    sleep_seconds(CLEAR_START_SECONDS);
    munmap(d->fb, d->fb_size);
    close(d->fd);
}

struct display *display_start(const struct display_config *cfg, struct frame_store *store,
                              const struct recorder *recorder, const struct http_server *srv)
{
    struct display *d = calloc(1, sizeof(*d));
    int rc;

    if (d == NULL) {
        syslog(LOG_ERR, "Out of memory");
        return NULL;
    }
    d->store = store;
    d->recorder = recorder;
    d->srv = srv;
    atomic_init(&d->stop, 0);

    if (fb_open(d, cfg->device) == -1) {
        free(d);
        return NULL;
    }
    if (choose_layout(d, cfg->width, cfg->height) == -1) {
        syslog(LOG_WARNING, "%ux%u frames don't fit the %ux%u display; running without it",
               cfg->width, cfg->height, d->xres, d->yres);
        goto err_fb;
    }
    d->back = calloc((size_t)d->xres * d->yres, sizeof(uint16_t));
    if (d->back == NULL) {
        syslog(LOG_ERR, "Out of memory for the display buffer");
        goto err_fb;
    }

    memset(d->fb, 0, d->fb_size);
    update_status(d);

    rc = thread_create_signals_blocked(&d->thread, display_thread, d);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_create for the display failed: %s", strerror(rc));
        free(d->back);
        goto err_fb;
    }
    syslog(LOG_INFO, "Display %s: %ux%u, camera image %ux%u at %u/8 scale", cfg->device,
           d->xres, d->yres, d->img_w, d->img_h, d->scale_num);
    return d;

err_fb:
    fb_close(d);
    free(d);
    return NULL;
}

void display_stop(struct display *d)
{
    if (d == NULL) {
        return;
    }
    atomic_store(&d->stop, 1);
    pthread_join(d->thread, NULL);
    fb_close(d);
    free(d->back);
    free(d);
}
