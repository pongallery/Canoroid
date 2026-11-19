/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "print.h"

void *cr_jpeg_alloc(size_t size)
{
    return malloc(size);
}

void cr_jpeg_free(void *ptr)
{
    free(ptr);
}

void *cr_open(const char *path, int write)
{
    return fopen(path, write ? "wbx" : "rb");
}

int cr_seek(void *file, unsigned offset)
{
    return fseek(file, offset, SEEK_SET) == 0;
}

int cr_read(void *file, void *buffer, size_t count)
{
    size_t read = fread(buffer, 1, count, file);
    return ferror(file) ? -1 : (int) read;
}

int cr_write(void *file, const void *buffer, size_t count)
{
    return (int) fwrite(buffer, 1, count, file);
}

int cr_close(void *file)
{
    return fclose(file) == 0;
}

int cr_exists(const char *path)
{
    struct stat attributes;
    if (lstat(path, &attributes) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

int cr_publish(const char *temporary, const char *destination)
{
    if (link(temporary, destination) != 0)
        return 0;
    if (unlink(temporary) == 0)
        return 1;
    unlink(destination);
    return 0;
}

int cr_remove(const char *path)
{
    return unlink(path) == 0;
}

int main(int argc, char **argv)
{
    if ((argc != 4 && argc != 5) ||
        (argc == 5 && (strlen(argv[4]) != 1 || argv[4][0] < '0' || argv[4][0] > '2')) || strlen(argv[3]) != 1 || argv[3][0] < '1' || argv[3][0] > '0' + CR_FRAME_COUNT) {
        fprintf(stderr, "Usage: %s input.jpg output.jpg frame-number[1-6] [tone 0-2]\n", argv[0]);
        return 2;
    }
    char error[96];
    char *temporary = malloc(strlen(argv[2]) + sizeof(".tmp"));
    unsigned char *preview = malloc(CR_PREVIEW_SIZE * CR_PREVIEW_SIZE * 3);
    if (!preview || !temporary) {
        fprintf(stderr, "%s: workspace allocation failed\n", argv[1]);
        free(preview);
        free(temporary);
        return 1;
    }
    strcpy(temporary, argv[2]);
    strcat(temporary, ".tmp");
    struct cr_frame style = cr_frames[argv[3][0] - '1'];
    style.tone = argc == 5 ? argv[4][0] - '0' : 0;
    int ok = cr_print(argv[1], temporary, argv[2], &style,
                      preview, NULL, NULL, error);
    if (!ok)
        fprintf(stderr, "%s: %s\n", argv[1], error);
    free(preview);
    free(temporary);
    return ok ? 0 : 1;
}
