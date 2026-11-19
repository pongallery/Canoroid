/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <jversion.h>
#include <jerror.h>
#include "print.h"

const struct cr_frame cr_frames[3] = {
    { "Classic White", {255, 255, 255}, 112, 112, 780, 4, 0 },
    { "Warm Border", {241, 212, 170}, 72, 42, 936, 0, 0 },
    { "Even Border", {240, 247, 239}, 54, 54, 972, 0, 0 },
};

struct cr_error {
    struct jpeg_error_mgr pub;
    void *jump[5];
    const char *stage;
    char *message;
};

struct cr_source {
    struct jpeg_source_mgr pub;
    void *file;
    JOCTET buffer[CR_IO_CHUNK];
};

struct cr_destination {
    struct jpeg_destination_mgr pub;
    void *file;
    JOCTET buffer[CR_IO_CHUNK];
};

struct cr_job {
    struct jpeg_decompress_struct decoder;
    struct jpeg_compress_struct encoder;
    struct cr_source source;
    struct cr_destination destination;
    struct cr_error error;
    char captured[20];
};

static const char *const jpeg_message[] = {
#define JMESSAGE(code, string) string,
#include <jerror.h>
    NULL
};

static void fail(j_common_ptr jpeg, const char *reason)
{
    struct cr_error *error = (void *) jpeg->err;
    snprintf(error->message, 96, "%s: %s", error->stage, reason);
    __builtin_longjmp(error->jump, 1);
}

static void jpeg_fatal(j_common_ptr jpeg)
{
    int code = jpeg->err->msg_code;
    const char *reason = "JPEG codec error";
    if (code > 0 && code < JMSG_LASTMSGCODE)
        reason = jpeg_message[code];
    if (strchr(reason, '%')) {
        char detail[72];
        if (strstr(reason, "%s")) {
            snprintf(detail, sizeof(detail), reason, jpeg->err->msg_parm.s);
        } else {
            const int *v = jpeg->err->msg_parm.i;
            snprintf(detail, sizeof(detail), reason, v[0], v[1], v[2], v[3],
                     v[4], v[5], v[6], v[7]);
        }
        fail(jpeg, detail);
    }
    fail(jpeg, reason);
}

static void jpeg_notice(j_common_ptr jpeg, int level)
{
    if (level < 0)
        jpeg_fatal(jpeg);
}

static void reset_error(j_common_ptr jpeg)
{
    jpeg->err->num_warnings = 0;
    jpeg->err->msg_code = 0;
}

static void noop_source(j_decompress_ptr jpeg)
{
    (void) jpeg;
}

static boolean refill(j_decompress_ptr jpeg)
{
    struct cr_source *source = (void *) jpeg->src;
    int count = cr_read(source->file, source->buffer, sizeof(source->buffer));
    if (count < 0)
        fail((j_common_ptr) jpeg, "card read failed");
    if (!count)
        fail((j_common_ptr) jpeg, "unexpected end of JPEG");
    source->pub.next_input_byte = source->buffer;
    source->pub.bytes_in_buffer = count;
    return TRUE;
}

static void skip(j_decompress_ptr jpeg, long count)
{
    if (count <= 0)
        return;
    while ((unsigned long) count > jpeg->src->bytes_in_buffer) {
        count -= jpeg->src->bytes_in_buffer;
        refill(jpeg);
    }
    jpeg->src->next_input_byte += count;
    jpeg->src->bytes_in_buffer -= count;
}

static void init_destination(j_compress_ptr jpeg)
{
    struct cr_destination *destination = (void *) jpeg->dest;
    destination->pub.next_output_byte = destination->buffer;
    destination->pub.free_in_buffer = sizeof(destination->buffer);
}

static void write_destination(j_compress_ptr jpeg, size_t count)
{
    struct cr_destination *destination = (void *) jpeg->dest;
    if (count && cr_write(destination->file, destination->buffer, count) != (int) count)
        fail((j_common_ptr) jpeg, "card write incomplete");
}

static boolean flush_destination(j_compress_ptr jpeg)
{
    struct cr_destination *destination = (void *) jpeg->dest;
    write_destination(jpeg, sizeof(destination->buffer));
    init_destination(jpeg);
    return TRUE;
}

static void finish_destination(j_compress_ptr jpeg)
{
    struct cr_destination *destination = (void *) jpeg->dest;
    write_destination(jpeg, sizeof(destination->buffer) - destination->pub.free_in_buffer);
}

static void start_decoder(struct cr_job *job, void *file, unsigned target)
{
    j_decompress_ptr jpeg = &job->decoder;
    job->source = (struct cr_source) {
        .pub = {
            .init_source = noop_source, .fill_input_buffer = refill,
            .skip_input_data = skip, .resync_to_restart = jpeg_resync_to_restart,
            .term_source = noop_source,
        },
        .file = file,
    };
    jpeg_create_decompress(jpeg);
    jpeg->src = &job->source.pub;
    jpeg_read_header(jpeg, TRUE);
    if (jpeg->arith_code || jpeg_has_multiple_scans(jpeg) ||
        jpeg->image_width > 8192 || jpeg->image_height > 8192 ||
        (jpeg->num_components != 1 && jpeg->num_components != 3))
        fail((j_common_ptr) jpeg, "unsupported JPEG format");
    /* Choose the smallest native decode that still covers the output crop. */
    unsigned short_side = jpeg->image_width < jpeg->image_height ? jpeg->image_width : jpeg->image_height;
    if (jpeg->block_size == DCTSIZE) {
        jpeg->scale_num = 1;
        jpeg->scale_denom = 1;
        if (short_side >= 4 * target)
            jpeg->scale_denom = 4;
        else if (short_side >= 2 * target)
            jpeg->scale_denom = 2;
    }
    jpeg->out_color_space = JCS_RGB;
    jpeg->dct_method = JDCT_ISLOW;
    jpeg_start_decompress(jpeg);
}

static unsigned sample(unsigned position, unsigned source, unsigned target)
{
    return ((2 * position + 1) * source) / (2 * target);
}

static JSAMPARRAY rows(j_common_ptr jpeg, unsigned width)
{
    return jpeg->mem->alloc_sarray(jpeg, JPOOL_IMAGE, width * 3, 1);
}

/* Sample the full original into a letterboxed comparison thumbnail as the
 * same scanlines pass through the encoder. */
static void read_row(j_decompress_ptr decoder, JSAMPARRAY row, unsigned char *original)
{
    unsigned y = decoder->output_scanline;
    jpeg_read_scanlines(decoder, row, 1);
    if (!original) return;
    unsigned w = decoder->output_width, h = decoder->output_height;
    unsigned longest = w > h ? w : h;
    unsigned pw = (w * CR_PREVIEW_SIZE + longest - 1) / longest;
    unsigned ph = (h * CR_PREVIEW_SIZE + longest - 1) / longest;
    unsigned first = (y * ph + h - 1) / h;
    unsigned end = ((y + 1) * ph + h - 1) / h;
    for (unsigned dy = first; dy < end; ++dy) {
        unsigned offset = ((CR_PREVIEW_SIZE - ph) / 2 + dy) * CR_PREVIEW_SIZE +
                          (CR_PREVIEW_SIZE - pw) / 2;
        for (unsigned x = 0; x < pw; ++x)
            memcpy(original + 3 * (offset + x), row[0] + 3 * (x * w / pw), 3);
    }
}

/* Read only DateTimeOriginal. Offsets in EXIF are relative to the TIFF
 * header; every lookup must stay inside this APP1, including on old cards. */
static unsigned exif_word(const unsigned char *p, int little)
{
    return little ? p[0] | (p[1] << 8) : (p[0] << 8) | p[1];
}

static unsigned exif_long(const unsigned char *p, int little)
{
    unsigned a = exif_word(p, little), b = exif_word(p + 2, little);
    return little ? a | (b << 16) : (a << 16) | b;
}

static int read_at(void *file, unsigned offset, void *buf, unsigned size)
{
    return cr_seek(file, offset) && cr_read(file, buf, size) == (int)size;
}

static int exif_date(void *file, unsigned base, unsigned size, char date[20])
{
    unsigned char hdr[12];
    if (size < 8 || !read_at(file, base, hdr, 8)) return 0;
    int little = hdr[0] == 'I';
    if ((!little && hdr[0] != 'M') || hdr[1] != hdr[0] || exif_word(hdr+2, little) != 42) return 0;
    unsigned offset = exif_long(hdr+4, little);
    for (int level = 0; level < 2; level++) {
        if (offset < 8 || offset > size - 2 || !read_at(file, base + offset, hdr, 2)) return 0;
        unsigned count = exif_word(hdr, little);
        if (count > (size - offset - 2) / 12 || count > 512) return 0;
        unsigned next = 0;
        for (unsigned i = 0; i < count; i++) {
            if (!read_at(file, base + offset + 2 + 12*i, hdr, 12)) return 0;
            unsigned tag = exif_word(hdr, little), type = exif_word(hdr+2, little);
            unsigned items = exif_long(hdr+4, little), value = exif_long(hdr+8, little);
            if (!level && tag == 0x8769 && type == 4 && items == 1) { next = value; break; }
            if (level && tag == 0x9003 && type == 2 && items == 20 &&
                value <= size && size - value >= 20) {
                char raw[20];
                if (!read_at(file, base + value, raw, 20) || raw[19]) return 0;
                const char *pattern = "0000:00:00 00:00:00";
                for (int j = 0; j < 19; j++) {
                    if (pattern[j] == '0' ? raw[j] < '0' || raw[j] > '9' : raw[j] != pattern[j]) return 0;
                }
                memcpy(date, raw, 20);
                return 1;
            }
        }
        if (!next) return 0;
        offset = next;
    }
    return 0;
}

void cr_capture_date(const char *path, char date[20])
{
    date[0] = 0;
    void *file = cr_open(path, 0);
    if (!file) return;
    unsigned char header[6];
    if (read_at(file, 0, header, 2) && header[0] == 255 && header[1] == 216) {
        unsigned pos = 2;
        for (int i = 0; i < 128; i++) {
            if (!read_at(file, pos, header, 4) || header[0] != 255 ||
                header[1] == 218 || header[1] == 217) break;
            if (header[1] == 255) { pos++; continue; }
            unsigned len = exif_word(header + 2, 0);
            if (len < 2) break;
            if (header[1] == 225 && len >= 16 && read_at(file, pos+4, header, 6) &&
                !memcmp(header, "Exif\0\0", 6) && exif_date(file, pos+10, len-8, date)) break;
            pos += 2 + len;
        }
    }
    cr_close(file);
}

static void write_capture_date(j_compress_ptr encoder, const char date[20])
{
    if (!date[0]) return;
    /* Small EXIF block containing only ExifIFDPointer and DateTimeOriginal. */
    unsigned char marker[70] = {
        'E','x','i','f',0,0, 'I','I',42,0,8,0,0,0,
        1,0, 0x69,0x87,4,0,1,0,0,0,26,0,0,0, 0,0,0,0,
        1,0, 3,0x90,2,0,20,0,0,0,44,0,0,0, 0,0,0,0
    };
    memcpy(marker + 50, date, 20);
    jpeg_write_marker(encoder, JPEG_APP0 + 1, marker, sizeof(marker));
}

static int encode(struct cr_job *job, void *input, void *output,
                  const struct cr_frame *frame, unsigned char *original, void (*progress)(unsigned))
{
    if (__builtin_setjmp(job->error.jump))
        return 0;
    job->error.stage = "Input JPEG";
    j_decompress_ptr decoder = &job->decoder;
    j_compress_ptr encoder = &job->encoder;
    unsigned inner = CR_PRINT_SIZE - 2 * frame->inset;
    start_decoder(job, input, CR_PRINT_SIZE);
    unsigned width = decoder->output_width;
    unsigned height = decoder->output_height;
    unsigned side = width < height ? width : height;
    unsigned left = (width - side) / 2;
    unsigned top = (height - side) / 2;
    JSAMPARRAY source = rows((j_common_ptr) decoder, width);

    job->error.stage = "Write framed JPEG";
    jpeg_create_compress(encoder);
    job->destination.pub.init_destination = init_destination;
    job->destination.pub.empty_output_buffer = flush_destination;
    job->destination.pub.term_destination = finish_destination;
    job->destination.file = output;
    encoder->dest = &job->destination.pub;
    encoder->image_width = CR_PRINT_SIZE;
    encoder->image_height = CR_PRINT_SIZE;
    encoder->input_components = 3;
    encoder->in_color_space = JCS_RGB;
    jpeg_set_defaults(encoder);
    jpeg_set_quality(encoder, 90, TRUE);
    jpeg_start_compress(encoder, TRUE);
    write_capture_date(encoder, job->captured);
    JSAMPARRAY output_row = rows((j_common_ptr) encoder, CR_PRINT_SIZE);
    unsigned *columns = (*encoder->mem->alloc_small)((j_common_ptr) encoder,
                                                     JPOOL_IMAGE, inner * sizeof(*columns));
    for (unsigned x = 0; x < inner; ++x)
        columns[x] = 3 * (left + sample(frame->inset + x, side, CR_PRINT_SIZE));

    for (unsigned y = 0; y < CR_PRINT_SIZE; ++y) {
        if (y == 0 || y == frame->top + frame->height + frame->outline) {
            for (unsigned x = 0; x < CR_PRINT_SIZE; ++x)
                memcpy(output_row[0] + x * 3, frame->rgb, 3);
        }
        if (frame->outline && (y + frame->outline == frame->top ||
                               y == frame->top + frame->height))
            memset(output_row[0] + 3 * (frame->inset - frame->outline), 0,
                   3 * (inner + 2 * frame->outline));
        if (y >= frame->top && y < frame->top + frame->height) {
            unsigned input_y = top + sample(y, side, CR_PRINT_SIZE);
            job->error.stage = "Input JPEG";
            while (decoder->output_scanline <= input_y)
                read_row(decoder, source, original);
            for (unsigned x = 0; x < inner; ++x)
                memcpy(output_row[0] + (frame->inset + x) * 3, source[0] + columns[x], 3);
        }
        if (frame->monochrome && y >= frame->top && y < frame->top + frame->height) {
            for (unsigned x = frame->inset; x < frame->inset + inner; ++x) {
                unsigned char *p = output_row[0] + 3 * x;
                unsigned gray = (77*p[0] + 150*p[1] + 29*p[2]) >> 8;
                memset(p, gray, 3);
            }
        }
        job->error.stage = "Write framed JPEG";
        jpeg_write_scanlines(encoder, output_row, 1);
        if (progress && y % 36 == 0)
            progress(5 + y * 80 / CR_PRINT_SIZE);
    }
    job->error.stage = "Input JPEG";
    while (decoder->output_scanline < height)
        read_row(decoder, source, original);
    jpeg_finish_decompress(decoder);
    job->error.stage = "Write framed JPEG";
    jpeg_finish_compress(encoder);
    return 1;
}

static int read_preview(struct cr_job *job, void *file, unsigned char *preview)
{
    if (__builtin_setjmp(job->error.jump))
        return 0;
    job->error.stage = "Verify saved JPEG";
    start_decoder(job, file, CR_PREVIEW_SIZE);
    j_decompress_ptr decoder = &job->decoder;
    if (decoder->image_width != CR_PRINT_SIZE || decoder->image_height != CR_PRINT_SIZE)
        fail((j_common_ptr) decoder, "incorrect dimensions");
    unsigned width = decoder->output_width;
    JSAMPARRAY source = rows((j_common_ptr) decoder, width);
    for (unsigned y = 0; y < CR_PREVIEW_SIZE; ++y) {
        unsigned input_y = sample(y, width, CR_PREVIEW_SIZE);
        while (decoder->output_scanline <= input_y)
            jpeg_read_scanlines(decoder, source, 1);
        for (unsigned x = 0; x < CR_PREVIEW_SIZE; ++x)
            memcpy(preview + (y * CR_PREVIEW_SIZE + x) * 3,
                   source[0] + 3 * sample(x, width, CR_PREVIEW_SIZE), 3);
    }
    while (decoder->output_scanline < decoder->output_height)
        jpeg_read_scanlines(decoder, source, 1);
    jpeg_finish_decompress(decoder);
    return 1;
}

static struct cr_job *new_job(char error[96])
{
    struct cr_job *job = cr_jpeg_alloc(sizeof(*job));
    if (!job) {
        snprintf(error, 96, "JPEG workspace allocation failed");
        return NULL;
    }

    memset(job, 0, sizeof(*job));
    job->error.pub.error_exit = jpeg_fatal;
    job->error.pub.emit_message = jpeg_notice;
    job->error.pub.reset_error_mgr = reset_error;
    job->error.message = error;
    job->decoder.err = &job->error.pub;
    job->encoder.err = &job->error.pub;
    return job;
}

int cr_load_print(const char *path, unsigned char *preview, char error[96])
{
    struct cr_job *job = new_job(error);
    if (!job) return 0;
    void *file = cr_open(path, 0);
    int ok = 0;
    if (file) {
        ok = read_preview(job, file, preview);
        cr_close(file);
    } else {
        snprintf(error, 96, "Open saved JPEG failed");
    }
    jpeg_destroy_decompress(&job->decoder);
    cr_jpeg_free(job);
    return ok;
}

int cr_print(const char *source, const char *temporary, const char *destination,
             const struct cr_frame *frame, unsigned char *preview, unsigned char *original,
             void (*progress)(unsigned), char error[96])
{
    int tmp_exists = cr_exists(temporary), dst_exists = cr_exists(destination);
    if (tmp_exists < 0 || dst_exists < 0) {
        snprintf(error, 96, "Cannot inspect output directory");
        return 0;
    }
    if (tmp_exists || dst_exists) {
        snprintf(error, 96, "Output path already exists");
        return 0;
    }
    struct cr_job *job = new_job(error);
    if (!job) return 0;
    cr_capture_date(source, job->captured);
    void *input = cr_open(source, 0);
    void *output = NULL;
    int created = 0;
    int ok = 0;
    error[0] = 0;
    if (!input) {
        snprintf(error, 96, "Open input: JPEG is missing or unreadable");
        goto done;
    }
    output = cr_open(temporary, 1);
    if (!output) {
        snprintf(error, 96, "Cannot create %s", temporary);
        goto done;
    }
    created = 1;
    int encoded = encode(job, input, output, frame, original, progress);
    int closed = cr_close(output);
    cr_close(input);
    input = output = NULL;
    jpeg_destroy_compress(&job->encoder);
    jpeg_destroy_decompress(&job->decoder);
    if (!encoded)
        goto done;
    if (!closed) {
        snprintf(error, 96, "Close temporary JPEG failed");
        goto done;
    }
    if (progress)
        progress(90);
    input = cr_open(temporary, 0);
    if (!input) {
        snprintf(error, 96, "Reopen temporary JPEG failed");
        goto done;
    }
    int verified = read_preview(job, input, preview);
    cr_close(input);
    input = NULL;
    if (!verified)
        goto done;
    if (cr_exists(destination) || !cr_publish(temporary, destination)) {
        snprintf(error, 96, "Publish framed JPEG failed");
        goto done;
    }
    created = 0;
    input = cr_open(destination, 0);
    if (!input) {
        snprintf(error, 96, "Reopen saved JPEG failed after publish");
        goto done;
    }
    ok = 1;
    if (progress)
        progress(100);

done:
    jpeg_destroy_compress(&job->encoder);
    jpeg_destroy_decompress(&job->decoder);
    if (input)
        cr_close(input);
    if (output)
        cr_close(output);
    if (created && !cr_remove(temporary)) {
        size_t length = strlen(error);
        snprintf(error + length, 96 - length, "; cleanup failed");
    }
    cr_jpeg_free(job);
    return ok;
}
