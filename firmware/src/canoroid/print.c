/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <jversion.h>
#include <jerror.h>
#include "print.h"

const struct cr_frame cr_frames[CR_FRAME_COUNT] = {
    { "Classic White", {255, 255, 255}, 112, 112, 780, 4, 0, 0, 0 },
    { "Warm Border", {241, 212, 170}, 72, 42, 936, 0, 0, 0, 0 },
    { "Even Border", {240, 247, 239}, 54, 54, 972, 0, 0, 0, 0 },
    { "Confetti", {255, 229, 148}, 75, 68, 795, 0, 0, 1, 0 },
    { "Mint Waves", {31, 153, 137}, 68, 60, 830, 0, 0, 2, 0 },
    { "Berry Daisy", {180, 61, 117}, 65, 61, 775, 0, 0, 3, 0 },
    { "Air Mail", {249, 241, 219}, 70, 70, 818, 0, 0, 4, 0 },
    { "Blue Fern", {29, 66, 111}, 66, 54, 836, 0, 0, 5, 0 },
    { "Film Gate", {38, 33, 36}, 46, 96, 888, 0, 0, 6, 0 },
    { "No Frame", {0,0,0}, 0, 0, 0, 0, 0, 0, 0 },
};

/* Pattern coordinates are always in the 1080-square print space. */
const unsigned char *cr_paper(const struct cr_frame *f, int x, int y)
{
    static const unsigned char accents[][3] = {
        {224, 90, 113}, {37, 159, 142}, {150, 85, 169},
        {255, 222, 108}, {255, 244, 220}
    };
    if (f->pattern == 1) {
        int band = y / 64;
        int shifted = x + (band % 2) * 32;
        int dx = shifted % 64 - 32, dy = y % 64 - 32;
        if (dx*dx + dy*dy < 130)
            return accents[(shifted / 64 + band) % 3];
    } else if (f->pattern == 2) {
        int wave = x % 160 - 80;
        int stripe = (y + x + wave*wave / 180) % 96;
        if (stripe < 8) return accents[4];
        if (stripe >= 18 && stripe < 23) return accents[3];
    } else if (f->pattern == 3) {
        if (y >= 850) {
            int dx = x % 216 - 108, dy = y - 952;
            int ax = abs(dx), ay = abs(dy);
            if (dx*dx + dy*dy < 145) return accents[3];
            if ((ax-23)*(ax-23) + dy*dy < 330 ||
                dx*dx + (ay-23)*(ay-23) < 330) return accents[4];
        }
        if (x % 46 < 4 && y % 46 < 4) return accents[4];
    } else if (f->pattern == 4) {
        static const unsigned char inks[][3]={{187,66,60},{50,86,129},{122,140,146}};
        if (x<22 || x>=1058 || y<22 || y>=1058) {
            int stripe=((x+y)/30)%4;
            if (stripe==0 || stripe==2) return inks[stripe/2];
        }
        /* Three cancellation lines on the postcard margin. */
        if (x>=728 && x<1004 && y>=930 && y<1020) {
            int wave=abs(x%68-34)/3;
            int line=y-947-wave;
            if (line>=0 && line<40 && line%18<3) return inks[2];
        }
    } else if (f->pattern == 5 && y>=901 && y<=1050 && x>=80 && x<1000) {
        /* Paired leaflets arch out from each tilted stem. */
        int up=1048-y, dx=(x-80)%184-54-up/4;
        if (abs(dx)<=2) return accents[4];
        if (up>=14 && up<138) {
            int span=(151-up)/3;
            int distance=abs(dx);
            int rib=(up-distance/2+60)%24-12;
            if (distance<span && abs(rib)*span<5*(span-distance))
                return accents[4];
        }
    } else if (f->pattern == 6) {
        static const unsigned char amber[]={228,161,77};
        int edge=y<540 ? y : 1079-y;
        int hole=abs(x%90-45);
        if (edge>=25 && edge<60 && hole<=22 &&
            hole+abs(edge-42)<=36) return accents[4];
        if (edge>=73 && edge<=78 && x%180<41) return amber;
    }
    return f->rgb;
}

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
    JOCTET *bytes;
    size_t capacity;
    void *file;
    JOCTET buffer[CR_IO_CHUNK];
};

/* Cache the first interpolation axis for every possible red byte. */
struct cr_fast_tone {
    unsigned short cell[256], fraction[256];
    unsigned rgb[256][17][17];
};

struct cr_job {
    struct jpeg_decompress_struct decoder;
    struct jpeg_compress_struct encoder;
    struct cr_source source;
    struct cr_destination destination;
    struct cr_error error;
    unsigned char *tone;
    struct cr_fast_tone *fast;
    unsigned char (*gray)[32];
    char captured[20];
    unsigned saved_width, saved_height;
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
    destination->pub.next_output_byte = destination->bytes;
    destination->pub.free_in_buffer = destination->capacity;
}

static void write_destination(j_compress_ptr jpeg, size_t count)
{
    struct cr_destination *destination = (void *) jpeg->dest;
    if (count && cr_write(destination->file, destination->bytes, count) != (int) count)
        fail((j_common_ptr) jpeg, "card write incomplete");
}

static boolean flush_destination(j_compress_ptr jpeg)
{
    struct cr_destination *destination = (void *) jpeg->dest;
    write_destination(jpeg, destination->capacity);
    init_destination(jpeg);
    return TRUE;
}

static void finish_destination(j_compress_ptr jpeg)
{
    struct cr_destination *destination = (void *) jpeg->dest;
    write_destination(jpeg, destination->capacity - destination->pub.free_in_buffer);
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
    if (target && jpeg->block_size == DCTSIZE) {
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

/* RGB cube in red-fastest order. Interpolate with 8-bit fixed-point weights. */
static int mix(int a, int b, int f)
{
    return (a * (256-f) + b*f + 128) >> 8;
}

static void apply_tone(const unsigned char *table, unsigned char *rgb)
{
    int base = 0, fraction[3], stride = 3;
    for (int i = 0; i < 3; ++i) {
        int pos = (rgb[i] * 4096 + 127) / 255;
        int cell = pos == 4096 ? 15 : pos >> 8;
        base += cell * stride;
        fraction[i] = pos - cell * 256;
        stride *= 17;
    }
    for (int c = 0; c < 3; ++c) {
        int planes[2];
        for (int z = 0; z < 2; ++z) {
            const unsigned char *p = table + base + c + z*867;
            planes[z] = mix(mix(p[0],p[3],fraction[0]),
                            mix(p[51],p[54],fraction[0]),fraction[1]);
        }
        rgb[c] = mix(planes[0],planes[1],fraction[2]);
    }
}

static void prepare_fast_tone(struct cr_job *job)
{
    struct cr_fast_tone *f=job->fast;
    for (int v=0; v<256; ++v) {
        int pos=(v*4096+127)/255;
        f->cell[v]=v==255 ? 15 : pos>>8;
        f->fraction[v]=pos-256*f->cell[v];
    }
    for (int b=0; b<17; ++b)
        for (int g=0; g<17; ++g)
            for (int r=0; r<256; ++r) {
                const unsigned char *p=job->tone+3*(b*289+g*17+f->cell[r]);
                unsigned color=0;
                for (int c=0; c<3; ++c)
                    color |= (unsigned)mix(p[c],p[c+3],f->fraction[r]) << (8*c);
                f->rgb[r][b][g]=color;
            }
}

/* The red and blue products fit in separate 16-bit lanes, including
 * the rounding bias. Keep the same rounding at each interpolation axis. */
static inline __attribute__((always_inline)) unsigned mix_rgb(unsigned a,unsigned b,unsigned weight)
{
    const unsigned rb=0x00ff00ff;
    unsigned pair=((a&rb)*(256-weight)+(b&rb)*weight+0x00800080)>>8;
    unsigned green=((a&0xff00)*(256-weight)+(b&0xff00)*weight+0x8000)>>8;
    return (pair&rb)|(green&0xff00);
}

static void fast_tone_row(const struct cr_fast_tone *f,unsigned char *row,unsigned width)
{
    for (unsigned x=0; x<width; ++x,row+=3) {
        unsigned g=row[1], b=row[2];
        const unsigned (*plane)[17]=f->rgb[row[0]]+f->cell[b];
        unsigned gi=f->cell[g], gf=f->fraction[g];
        unsigned low=mix_rgb(plane[0][gi],plane[0][gi+1],gf);
        unsigned high=mix_rgb(plane[1][gi],plane[1][gi+1],gf);
        unsigned color=mix_rgb(low,high,f->fraction[b]);
        row[0]=color;
        row[1]=color>>8;
        row[2]=color>>16;
    }
}

static unsigned char noir_value(int luma,int noise)
{
    int gray=128+(luma-128)*5/4+noise*(255-abs(2*luma-255))/255;
    return gray<0 ? 0 : gray>255 ? 255 : gray;
}

static int encode(struct cr_job *job, void *input, void *output,
                  const struct cr_frame *frame, unsigned char *original, void (*progress)(unsigned), int *shift_tenths)
{
    if (__builtin_setjmp(job->error.jump))
        return 0;
    job->error.stage = "Input JPEG";
    j_decompress_ptr decoder = &job->decoder;
    j_compress_ptr encoder = &job->encoder;
    unsigned inner = CR_PRINT_SIZE - 2 * frame->inset;
    int full = frame->height == 0;
    start_decoder(job, input, full ? 0 : CR_PRINT_SIZE);
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
    struct cr_destination *dest=&job->destination;
    dest->bytes=full ? cr_jpeg_alloc(32768) : NULL;
    dest->capacity=32768;
    if (!dest->bytes) {
        dest->bytes=dest->buffer;
        dest->capacity=sizeof(dest->buffer);
    }
    encoder->dest = &job->destination.pub;
    unsigned out_w = full ? width : CR_PRINT_SIZE;
    unsigned out_h = full ? height : CR_PRINT_SIZE;
    if (full) inner=width;
    unsigned photo_end = full ? height : frame->top + frame->height;
    encoder->image_width = job->saved_width = out_w;
    encoder->image_height = job->saved_height = out_h;
    int gray_jpeg=full && frame->monochrome;
    encoder->input_components = gray_jpeg ? 1 : 3;
    encoder->in_color_space = gray_jpeg ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(encoder);
    jpeg_set_quality(encoder, 90, TRUE);
    jpeg_start_compress(encoder, TRUE);
    write_capture_date(encoder, job->captured);
    JSAMPARRAY output_row = full && !shift_tenths ? source : rows((j_common_ptr) encoder, out_w);
    unsigned *columns = NULL;
    if (!full) {
        columns = (*encoder->mem->alloc_small)((j_common_ptr)encoder,JPOOL_IMAGE,inner*sizeof(*columns));
        for (unsigned x=0; x<inner; ++x)
            columns[x]=3*(left+sample(frame->inset+x,side,CR_PRINT_SIZE));
    }

    if (full && job->tone) {
        job->fast=cr_jpeg_alloc(sizeof(*job->fast));
        if (job->fast) prepare_fast_tone(job);
    }
    if (full && frame->monochrome) {
        job->gray=cr_jpeg_alloc(256*sizeof(*job->gray));
        if (job->gray)
            for (int l=0;l<256;++l)
                for (int n=0;n<32;++n) job->gray[l][n]=noir_value(l,n-16);
    }
    unsigned difference = 0, channels = 0;
    unsigned grain = 3200;
    for (unsigned y = 0; y < out_h; ++y) {
        if (frame->pattern) {
            int inside = y >= frame->top && y < photo_end;
            unsigned left_end = inside ? frame->inset : CR_PRINT_SIZE;
            for (unsigned x = 0; x < left_end; ++x)
                memcpy(output_row[0] + 3*x, cr_paper(frame, x, y), 3);
            if (inside)
                for (unsigned x = frame->inset + inner; x < CR_PRINT_SIZE; ++x)
                    memcpy(output_row[0] + 3*x, cr_paper(frame, x, y), 3);
        } else if (!full && (y == 0 || y == photo_end + frame->outline)) {
            for (unsigned x = 0; x < CR_PRINT_SIZE; ++x)
                memcpy(output_row[0] + x * 3, frame->rgb, 3);
        }
        if (frame->outline && (y + frame->outline == frame->top ||
                               y == frame->top + frame->height))
            memset(output_row[0] + 3 * (frame->inset - frame->outline), 0,
                   3 * (inner + 2 * frame->outline));
        if (y >= frame->top && y < photo_end) {
            unsigned input_y = full ? y : top + sample(y, side, CR_PRINT_SIZE);
            job->error.stage = "Input JPEG";
            while (decoder->output_scanline <= input_y)
                read_row(decoder, source, original);
            if (full && output_row!=source) memcpy(output_row[0],source[0],3*width);
            if (!full) for (unsigned x = 0; x < inner; ++x)
                memcpy(output_row[0] + (frame->inset + x) * 3, source[0] + columns[x], 3);
        }
        if (job->fast) fast_tone_row(job->fast,output_row[0],width);
        else if (job->tone && y >= frame->top && y < photo_end)
            for (unsigned x = frame->inset; x < frame->inset + inner; ++x)
                apply_tone(job->tone, output_row[0] + 3*x);
        if (frame->monochrome && y >= frame->top && y < photo_end) {
            for (unsigned x = frame->inset; x < frame->inset + inner; ++x) {
                unsigned char *p = output_row[0] + 3 * x;
                int luma = (77*p[0] + 150*p[1] + 29*p[2]) >> 8;
                grain = grain * 1664525u + 1013904223u;
                unsigned n=grain>>27;
                unsigned char value=job->gray ? job->gray[luma][n] : noir_value(luma,(int)n-16);
                if (gray_jpeg) output_row[0][x]=value;
                else p[0]=p[1]=p[2]=value;
            }
        }
        job->error.stage = "Write framed JPEG";
        if (shift_tenths && !(y % 16) && y >= frame->top && y < photo_end) {
            for (unsigned x = 0; x < inner; x += 16) {
                const unsigned char *before = source[0] + (full ? 3*x : columns[x]);
                const unsigned char *after = output_row[0] + 3*(frame->inset + x);
                for (int c = 0; c < 3; ++c) difference += abs((gray_jpeg ? output_row[0][x] : after[c]) - before[c]);
                channels += 3;
            }
        }
        jpeg_write_scanlines(encoder, output_row, 1);
        if (progress && y % (out_h/30+1) == 0)
            progress(5 + y * 80 / out_h);
    }
    job->error.stage = "Input JPEG";
    while (decoder->output_scanline < height)
        read_row(decoder, source, original);
    jpeg_finish_decompress(decoder);
    job->error.stage = "Write framed JPEG";
    if (progress) progress(85);
    jpeg_finish_compress(encoder);
    if (shift_tenths && channels) *shift_tenths = (difference*10 + channels/2)/channels;
    return 1;
}

static int read_preview(struct cr_job *job, void *file, unsigned char *preview)
{
    if (__builtin_setjmp(job->error.jump))
        return 0;
    job->error.stage = "Verify saved JPEG";
    start_decoder(job, file, CR_PREVIEW_SIZE);
    j_decompress_ptr decoder = &job->decoder;
    if (job->saved_width && (decoder->image_width != job->saved_width ||
                             decoder->image_height != job->saved_height))
        fail((j_common_ptr) decoder, "incorrect dimensions");
    JSAMPARRAY source = rows((j_common_ptr) decoder, decoder->output_width);
    memset(preview, 0, 3*CR_PREVIEW_SIZE*CR_PREVIEW_SIZE);
    while (decoder->output_scanline < decoder->output_height)
        read_row(decoder, source, preview);
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
             void (*progress)(unsigned), int *shift_tenths, char error[96])
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
    if (frame->tone) {
        const char *path = frame->tone == 1 ? "ML/TONES/FADED.LUT" :
                           frame->tone == 2 ? "ML/TONES/VIVID.LUT" : "ML/TONES/REDSCALE.LUT";
        unsigned size = 17*17*17*3;
        job->tone = cr_jpeg_alloc(size);
        if (!job->tone) {
            snprintf(error, 96, "Tone memory allocation failed");
            goto done;
        }
        void *lut = cr_open(path, 0);
        unsigned offset = 0;
        while (lut && offset < size) {
            unsigned n = size-offset < CR_IO_CHUNK ? size-offset : CR_IO_CHUNK;
            int got = cr_read(lut, job->tone+offset, n);
            if (got <= 0) break;
            offset += got;
        }
        if (lut) cr_close(lut);
        if (offset != size) {
            snprintf(error, 96, "Cannot read tone: %s", path);
            goto done;
        }
    }
    output = cr_open(temporary, 1);
    if (!output) {
        snprintf(error, 96, "Cannot create %s", temporary);
        goto done;
    }
    created = 1;
    int encoded = encode(job, input, output, frame, original, progress, shift_tenths);
    int closed = cr_close(output);
    cr_close(input);
    input = output = NULL;
    jpeg_destroy_compress(&job->encoder);
    jpeg_destroy_decompress(&job->decoder);
    if (job->fast) cr_jpeg_free(job->fast);
    if (job->gray) cr_jpeg_free(job->gray);
    if (job->destination.bytes && job->destination.bytes!=job->destination.buffer)
        cr_jpeg_free(job->destination.bytes);
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
    if (job->tone) cr_jpeg_free(job->tone);
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
