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
};

extern const struct cr_frame cr_frames[3];

/* Paths are distinct and within a directory owned by Canoroid. The caller
 * serializes jobs and supplies CR_PREVIEW_SIZE squared RGB pixels. */
int cr_print(const char *source, const char *temporary, const char *destination,
             const struct cr_frame *frame, unsigned char *preview, unsigned char *original,
             void (*progress)(unsigned), char error[96]);

int cr_load_print(const char *path, unsigned char *preview, char error[96]);

void *cr_jpeg_alloc(size_t size);
void cr_jpeg_free(void *ptr);

/* I/O implementations return signed byte counts, or a negative error. */
void *cr_open(const char *path, int write);
int cr_read(void *file, void *buffer, size_t count);
int cr_write(void *file, const void *buffer, size_t count);
int cr_close(void *file);
int cr_exists(const char *path);
int cr_publish(const char *temporary, const char *destination);
int cr_remove(const char *path);

#endif
