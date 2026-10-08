/**
 * @file avi_writer.c
 * @brief Writes MJPEG frames into an AVI 1.0 file.
 *
 * File layout (all values little-endian):
 *
 *   RIFF <size> "AVI "
 *     LIST <size> "hdrl"
 *       "avih" <56>  main header: frame period, frame count, size
 *       LIST <size> "strl"
 *         "strh" <56>  stream header: "vids", "MJPG", rate, length
 *         "strf" <40>  BITMAPINFOHEADER
 *     LIST <size> "movi"
 *       "00dc" <len> JPEG data, padded to an even length    (one per frame)
 *     "idx1" <16 * frames>  one entry per frame: id, flags, offset from "movi", length
 *
 * The sizes and frame counts are unknown until the end, so avi_close() seeks back and writes
 * them. The offsets below locate those fields in the fixed size header.
 */

#include "avi_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define HEADER_SIZE             224     /* everything before the first frame chunk */
#define OFFSET_RIFF_SIZE        4
#define OFFSET_AVIH_TOTALFRAMES 48
#define OFFSET_AVIH_BUFFERSIZE  60
#define OFFSET_STRH_LENGTH      140
#define OFFSET_STRH_BUFFERSIZE  144
#define OFFSET_MOVI_SIZE        216
#define OFFSET_MOVI_FOURCC      220     /* index offsets are relative to this */

#define AVIF_HASINDEX           0x10
#define AVIIF_KEYFRAME          0x10
/* Large stdio buffer so frames reach the SD card in big writes */
#define WRITE_BUFFER_SIZE       (1024 * 1024)

struct index_entry {
    uint32_t offset;
    uint32_t size;
};

struct avi_writer {
    FILE *f;
    char *path;
    char *buffer;
    struct index_entry *index;
    unsigned long frames;
    unsigned long index_capacity;
    uint64_t movi_bytes;        /* frame chunks written, including headers and padding */
    uint32_t max_frame;
    int failed;                 /* a write failed; the file is closed without the index */
};

static void put_le16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void put_fourcc(unsigned char *p, const char *fourcc)
{
    memcpy(p, fourcc, 4);
}

static int write_bytes(struct avi_writer *w, const void *data, size_t len)
{
    if (fwrite(data, 1, len, w->f) != len) {
        syslog(LOG_ERR, "Write to %s failed: %m", w->path);
        w->failed = 1;
        return -1;
    }
    return 0;
}

/** Write a little-endian 32 bit value at a fixed offset in the header. */
static int patch_le32(struct avi_writer *w, long offset, uint32_t value)
{
    unsigned char b[4];

    put_le32(b, value);
    if (fseek(w->f, offset, SEEK_SET) != 0) {
        syslog(LOG_ERR, "Seek in %s failed: %m", w->path);
        return -1;
    }
    return write_bytes(w, b, sizeof(b));
}

static void build_header(unsigned char *h, unsigned int width, unsigned int height,
                         unsigned int fps)
{
    unsigned char *p = h;

    memset(h, 0, HEADER_SIZE);

    put_fourcc(p, "RIFF");                      /* size filled in on close */
    put_fourcc(p + 8, "AVI ");
    p += 12;

    put_fourcc(p, "LIST");
    put_le32(p + 4, 192);                       /* "hdrl" + avih chunk + strl list */
    put_fourcc(p + 8, "hdrl");
    p += 12;

    put_fourcc(p, "avih");
    put_le32(p + 4, 56);
    put_le32(p + 8, 1000000 / fps);             /* microseconds per frame */
    put_le32(p + 20, AVIF_HASINDEX);
    put_le32(p + 32, 1);                        /* streams */
    put_le32(p + 40, width);
    put_le32(p + 44, height);
    p += 64;

    put_fourcc(p, "LIST");
    put_le32(p + 4, 116);                       /* "strl" + strh chunk + strf chunk */
    put_fourcc(p + 8, "strl");
    p += 12;

    put_fourcc(p, "strh");
    put_le32(p + 4, 56);
    put_fourcc(p + 8, "vids");
    put_fourcc(p + 12, "MJPG");
    put_le32(p + 28, 1);                        /* scale: rate / scale = frames per second */
    put_le32(p + 32, fps);                      /* rate */
    put_le32(p + 48, 0xFFFFFFFF);               /* quality: default */
    put_le16(p + 60, (uint16_t)width);          /* frame rectangle: left, top 0, right, bottom */
    put_le16(p + 62, (uint16_t)height);
    p += 64;

    put_fourcc(p, "strf");
    put_le32(p + 4, 40);
    put_le32(p + 8, 40);                        /* BITMAPINFOHEADER size */
    put_le32(p + 12, width);
    put_le32(p + 16, height);
    put_le16(p + 20, 1);                        /* planes */
    put_le16(p + 22, 24);                       /* bits per pixel */
    put_fourcc(p + 24, "MJPG");
    put_le32(p + 28, width * height * 3);
    p += 48;

    put_fourcc(p, "LIST");                      /* size filled in on close */
    put_fourcc(p + 8, "movi");
}

struct avi_writer *avi_open(const char *path, unsigned int width, unsigned int height,
                            unsigned int fps)
{
    unsigned char header[HEADER_SIZE];
    struct avi_writer *w;
    int fd;

    w = calloc(1, sizeof(*w));
    if (w == NULL || (w->path = strdup(path)) == NULL ||
        (w->buffer = malloc(WRITE_BUFFER_SIZE)) == NULL) {
        syslog(LOG_ERR, "Out of memory opening %s", path);
        goto fail;
    }

    /* O_EXCL: never overwrite an existing clip */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd == -1) {
        if (errno != EEXIST) {
            syslog(LOG_ERR, "Cannot create %s: %m", path);
        }
        goto fail;
    }
    w->f = fdopen(fd, "wb");
    if (w->f == NULL) {
        syslog(LOG_ERR, "fdopen %s failed: %m", path);
        close(fd);
        goto fail;
    }
    setvbuf(w->f, w->buffer, _IOFBF, WRITE_BUFFER_SIZE);

    build_header(header, width, height, fps);
    if (write_bytes(w, header, sizeof(header)) == -1) {
        fclose(w->f);
        unlink(path);
        goto fail;
    }
    return w;

fail:
    if (w != NULL) {
        free(w->buffer);
        free(w->path);
        free(w);
    }
    return NULL;
}

int avi_write_frame(struct avi_writer *w, const void *jpeg, size_t len)
{
    static const unsigned char pad = 0;
    unsigned char chunk[8];

    if (w->failed) {
        return -1;
    }
    if (w->frames == w->index_capacity) {
        unsigned long capacity = w->index_capacity ? w->index_capacity * 2 : 1024;
        struct index_entry *index = realloc(w->index, capacity * sizeof(*index));

        if (index == NULL) {
            syslog(LOG_ERR, "Out of memory for the index of %s", w->path);
            w->failed = 1;
            return -1;
        }
        w->index = index;
        w->index_capacity = capacity;
    }

    put_fourcc(chunk, "00dc");                  /* stream 0, compressed video */
    put_le32(chunk + 4, (uint32_t)len);
    if (write_bytes(w, chunk, sizeof(chunk)) == -1 || write_bytes(w, jpeg, len) == -1 ||
        ((len & 1) && write_bytes(w, &pad, 1) == -1)) {
        return -1;
    }

    /* Offset of the chunk header, counted from the "movi" fourcc */
    w->index[w->frames].offset = (uint32_t)(4 + w->movi_bytes);
    w->index[w->frames].size = (uint32_t)len;
    w->frames++;
    w->movi_bytes += sizeof(chunk) + len + (len & 1);
    if (len > w->max_frame) {
        w->max_frame = (uint32_t)len;
    }
    return 0;
}

uint64_t avi_size(const struct avi_writer *w)
{
    return HEADER_SIZE + w->movi_bytes + 8 + (uint64_t)w->frames * 16;
}

unsigned long avi_frames(const struct avi_writer *w)
{
    return w->frames;
}

/** Write the idx1 chunk and fill in the header fields. */
static int finish(struct avi_writer *w)
{
    unsigned char entry[16];
    unsigned long i;

    put_fourcc(entry, "idx1");
    put_le32(entry + 4, (uint32_t)(w->frames * 16));
    if (write_bytes(w, entry, 8) == -1) {
        return -1;
    }
    for (i = 0; i < w->frames; i++) {
        put_fourcc(entry, "00dc");
        put_le32(entry + 4, AVIIF_KEYFRAME);    /* every MJPEG frame is a keyframe */
        put_le32(entry + 8, w->index[i].offset);
        put_le32(entry + 12, w->index[i].size);
        if (write_bytes(w, entry, sizeof(entry)) == -1) {
            return -1;
        }
    }

    if (patch_le32(w, OFFSET_RIFF_SIZE, (uint32_t)(avi_size(w) - 8)) == -1 ||
        patch_le32(w, OFFSET_MOVI_SIZE, (uint32_t)(4 + w->movi_bytes)) == -1 ||
        patch_le32(w, OFFSET_AVIH_TOTALFRAMES, (uint32_t)w->frames) == -1 ||
        patch_le32(w, OFFSET_AVIH_BUFFERSIZE, w->max_frame + 8) == -1 ||
        patch_le32(w, OFFSET_STRH_LENGTH, (uint32_t)w->frames) == -1 ||
        patch_le32(w, OFFSET_STRH_BUFFERSIZE, w->max_frame + 8) == -1) {
        return -1;
    }

    /* Make sure the clip is on the SD card, not only in the page cache */
    if (fflush(w->f) != 0 || fsync(fileno(w->f)) != 0) {
        syslog(LOG_ERR, "Flushing %s failed: %m", w->path);
        return -1;
    }
    return 0;
}

int avi_close(struct avi_writer *w)
{
    int ret = -1;

    if (!w->failed) {
        ret = finish(w);
    }
    if (fclose(w->f) != 0 && ret == 0) {
        syslog(LOG_ERR, "Closing %s failed: %m", w->path);
        ret = -1;
    }
    free(w->index);
    free(w->buffer);
    free(w->path);
    free(w);
    return ret;
}
