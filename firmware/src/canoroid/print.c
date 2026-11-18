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

static void start_decoder(struct cr_job *job, void *file)
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

static int encode(struct cr_job *job, void *input, void *output,
                  const struct cr_frame *frame, unsigned char *original, void (*progress)(unsigned))
{
    if (__builtin_setjmp(job->error.jump))
        return 0;
    job->error.stage = "Input JPEG";
    j_decompress_ptr decoder = &job->decoder;
    j_compress_ptr encoder = &job->encoder;
    unsigned inner = CR_PRINT_SIZE - 2 * frame->inset;
    start_decoder(job, input);
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
    JSAMPARRAY output_row = rows((j_common_ptr) encoder, CR_PRINT_SIZE);
    unsigned *columns = (*encoder->mem->alloc_small)((j_common_ptr) encoder,
                                                     JPOOL_IMAGE, inner * sizeof(*columns));
    for (unsigned x = 0; x < inner; ++x)
        columns[x] = 3 * (left + sample(frame->inset + x, side, CR_PRINT_SIZE));

    for (unsigned y = 0; y < CR_PRINT_SIZE; ++y) {
        for (unsigned x = 0; x < CR_PRINT_SIZE; ++x)
            memcpy(output_row[0] + x * 3, frame->rgb, 3);
        if (frame->outline && y + frame->outline >= frame->top &&
            y < frame->top + frame->height + frame->outline)
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
    start_decoder(job, file);
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
    if (cr_exists(temporary) || cr_exists(destination)) {
        snprintf(error, 96, "Output path already exists");
        return 0;
    }
    struct cr_job *job = new_job(error);
    if (!job) return 0;
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
        snprintf(error, 96, "Create temporary JPEG failed");
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
