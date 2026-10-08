/**
 * @file avi_writer.h
 * @brief Writes MJPEG frames into an AVI 1.0 file, which plays in VLC and most other players.
 *
 * The headers are written with placeholder lengths when the file is opened; frames are appended
 * as they arrive; avi_close() writes the frame index and fills in the lengths. AVI 1.0 files
 * should stay under 1 GB, so callers check avi_size() and start a new file before that.
 */

#ifndef AVI_WRITER_H
#define AVI_WRITER_H

#include <stddef.h>
#include <stdint.h>

#define AVI_MAX_SIZE    (1000u * 1000u * 1000u)

struct avi_writer;

/**
 * Create path (it must not exist) and write the AVI headers.
 * @return the writer, or NULL on error (logged to syslog)
 */
struct avi_writer *avi_open(const char *path, unsigned int width, unsigned int height,
                            unsigned int fps);

/**
 * Append one JPEG frame.
 * @return 0 on success, -1 on a write error (logged)
 */
int avi_write_frame(struct avi_writer *w, const void *jpeg, size_t len);

/** Bytes the file will have once closed with the frames written so far. */
uint64_t avi_size(const struct avi_writer *w);

unsigned long avi_frames(const struct avi_writer *w);

/**
 * Write the index, fill in the header lengths, flush the file to the SD card, close it and free
 * the writer.
 * @return 0 on success, -1 on a write error (logged); the writer is freed either way
 */
int avi_close(struct avi_writer *w);

#endif /* AVI_WRITER_H */
