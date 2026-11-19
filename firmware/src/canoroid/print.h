/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CANOROID_PRINT_H
#define CANOROID_PRINT_H

#include <stddef.h>

#define CR_PRINT_SIZE 1080
#define CR_PREVIEW_SIZE 240
#define CR_IO_CHUNK 4096

struct cr_frame {
    const char *name;
    unsigned char rgb[3];
    unsigned inset;
    unsigned top;
    unsigned height;
    unsigned outline;
    int monochrome;
    unsigned pattern;
    unsigned tone; /* 0 original, 1 Faded Film, 2 Vivid Pop, 3 Redscale 100 */
};

#define CR_FRAME_COUNT 10
#define CR_NO_FRAME 9
extern const struct cr_frame cr_frames[CR_FRAME_COUNT];
const unsigned char *cr_paper(const struct cr_frame *frame, int x, int y);

/* Paths are distinct and within a directory owned by Canoroid. The caller
 * serializes jobs and supplies CR_PREVIEW_SIZE squared RGB pixels. */
int cr_print(const char *source, const char *temporary, const char *destination,
             const struct cr_frame *frame, unsigned char *preview, unsigned char *original,
             void (*progress)(unsigned), int *shift_tenths, char error[96]);

void cr_capture_date(const char *path, char date[20]);
int cr_seek(void *file, unsigned offset);
int cr_load_print(const char *path, unsigned char *preview, char error[96]);

void *cr_jpeg_alloc(size_t size);
void cr_jpeg_free(void *ptr);

/* I/O implementations return signed byte counts, or a negative error. */
void *cr_open(const char *path, int write);
int cr_read(void *file, void *buffer, size_t count);
int cr_write(void *file, const void *buffer, size_t count);
int cr_close(void *file);
/* 1 if occupied, 0 if free, negative if the directory cannot be read. */
int cr_exists(const char *path);
int cr_publish(const char *temporary, const char *destination);
int cr_remove(const char *path);

#endif
