/* SPDX-License-Identifier: GPL-2.0-only */
#include "dryos.h"
#include "bmp.h"
#include "fio-ml.h"
#include "gui.h"
#include "gui-common.h"
#include "lens.h"
#include "task_utils.h"
#include "zebra.h"
#include "print.h"
#include "fileprefix.h"
#include "vram.h"

#define PRINT_DIR "ML/CANOROID"
static char source_path[100];
static unsigned char *original_preview;
static int compare_original;
static int mono;
static volatile int shooting, cancel_shot;

enum page { HOME, FRAMES, DEVELOPING, RESULT, GALLERY, DELETE_PRINT, FAILED };

static volatile int active;
static volatile int open_requested;
static volatile int leave_requested;
/* Canon's GUI task produces presses. The Canoroid task consumes them. */
#define KEY_SLOTS 8
static volatile int key_queue[KEY_SLOTS], key_read, key_write;
static volatile int opening;
static int delete_selected;
static uint8_t *last_presented;
static unsigned last_display_check;
static uint32_t last_signature;

/* Probe a grid across the displayed frame. Canon can redraw only part of
 * the bitmap without changing either the GUI mode or the buffer address. */
static uint32_t screen_signature(const uint8_t *bitmap)
{
    uint32_t hash = 5381;
    for (int y = 16; y < 480; y += 24)
        for (int x = 14; x < 720; x += 24)
            hash = hash * 33 + bitmap[y * BMPPITCH + x];
    return hash;
}

static void post_key(int button)
{
    int next = (key_write + 1) % KEY_SLOTS;
    if (next == key_read) return;
    key_queue[key_write] = button;
    key_write = next;
}

static int selection;
static int frame;
static int gui_mode;
static volatile enum page page;
static unsigned char *preview;
static unsigned char *card_read_buffer;
static char error[96];
static char print_name[13];
static unsigned gallery_position;
static int gallery_loaded;
#define PREVIEW_BYTES (CR_PREVIEW_SIZE * CR_PREVIEW_SIZE * 3)
static int retained_page;
static int waiting_jpeg;
static char log_status[64];
static const char *frame_hint;

static int usable_screen(void)
{
    return DISPLAY_IS_ON && !ml_shutdown_requested && !is_movie_mode() &&
           (CURRENT_GUI_MODE == 0 || CURRENT_GUI_MODE == GUIMODE_ML_MENU ||
            MENU_MODE || QR_MODE);
}

/* Quick Review and Live View can exchange GUI modes during a single print. */
static void claim_review_screen(void)
{
    if (!usable_screen()) return;
    gui_mode = CURRENT_GUI_MODE;
    if (!canon_gui_front_buffer_disabled()) canon_gui_disable_front_buffer();
    bmp_on();
}


void *cr_open(const char *path, int write)
{
    if (!write) return FIO_OpenFile(path, O_RDONLY | O_SYNC);
    /* DryOS can reject creation while Canon is finishing a card write.
     * FIO_CreateFile removes its target, so recheck before every attempt. */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) msleep(200);
        if (ml_shutdown_requested || cr_exists(path) != 0) return NULL;
        FILE *file = FIO_CreateFile(path);
        if (file) return file;
    }
    return NULL;
}

int cr_read(void *file, void *buffer, size_t count)
{
    if (!card_read_buffer || count > CR_IO_CHUNK)
        return -1;
    int bytes = FIO_ReadFile(file, card_read_buffer, count);
    if (bytes > 0)
        memcpy(buffer, card_read_buffer, bytes);
    return bytes;
}

int cr_write(void *file, const void *buffer, size_t count)
{
    return FIO_WriteFile(file, buffer, count);
}

int cr_close(void *file)
{
    /* Canon's wrapper exposes no close result. Reopening and decoding
     * the complete JPEG is the check after this call. */
    FIO_CloseFile(file);
    return 1;
}

int cr_exists(const char *path)
{
    /* All camera-side destinations belong to PRINT_DIR. FindFirstEx takes
     * a directory, not a proposed filename. Match files and directories so
     * an existing name (including an empty file) is never reused. */
    if (strncmp(path, PRINT_DIR "/", sizeof(PRINT_DIR)) != 0) return -1;
    if (is_file(path)) return 1;
    const char *name = path + sizeof(PRINT_DIR);
    struct fio_file entry;
    struct fio_dirent *dir = FIO_FindFirstEx(PRINT_DIR "/", &entry);
    if (!dir || IS_ERROR(dir)) return -1;
    int found = 0;
    do {
        if (!strcasecmp(name, entry.name)) {
            found = 1;
            break;
        }
    } while (FIO_FindNextEx(dir, &entry) == 0);
    FIO_FindClose(dir);
    return found;
}

int cr_publish(const char *temporary, const char *destination)
{
    return FIO_RenameFile(temporary, destination) == 0;
}

int cr_remove(const char *path)
{
    return FIO_RemoveFile(path) == 0;
}

/* Allocate JPEG pools through the same allocator as the application. */
void *cr_jpeg_alloc(size_t size)
{
    return malloc(size);
}

void cr_jpeg_free(void *ptr)
{
    free(ptr);
}

static int display_available(void)
{
    return DISPLAY_IS_ON && !ml_shutdown_requested &&
           CURRENT_GUI_MODE == gui_mode && canon_gui_front_buffer_disabled();
}

static void text(int font, int x, int y, const char *label)
{
    bmp_printf(font, x, y, "%s", label);
}

static void heading(const char *title, const char *controls)
{
    bmp_fill(COLOR_ALMOST_BLACK, 0, 0, 720, 480);
    bmp_fill(COLOR_ORANGE, 36, 30, 5, 29);
    text(FONT_LARGE, 54, 26, title);
    bmp_fill(COLOR_GRAY(55), 36, 426, 648, 2);
    text(FONT_MED_LARGE, 38, 434, controls);
}

static void camera_icon(int x, int y, int paper)
{
    int white = COLOR_ALMOST_WHITE;
    bmp_draw_rect_chamfer(white, x, y, 168, 103, 14, 1);
    bmp_draw_rect_chamfer(white, x + 49, y + 15, 66, 59, 14, 1);
    bmp_draw_rect(white, x + 126, y + 14, 22, 14);
    bmp_fill(COLOR_ORANGE, x + 23, y + 35, 9, 9);
    bmp_fill(white, x + 15, y + 80, 138, 3);
    if (paper) {
        bmp_fill(white, x + 20, y + 84, 128, paper);
        if (paper > 9)
            bmp_fill(COLOR_GRAY(38), x + 27, y + 90, 114, paper - 8);
    }
}

/* Scale the existing thumbnail at display time so each shot keeps the
 * same camera RAM budget. The framed JPEG on the card stays 1080 square. */
#define PHOTO_SIDE 360
static void render_preview(unsigned progress)
{
    uint8_t *vram = bmp_vram();
    const unsigned char *pixels = compare_original ? original_preview : preview;
    if (!vram || !pixels) return;
    for (unsigned y = 0; y < progress * PHOTO_SIDE / 100; ++y) {
        const unsigned char *row = pixels + 3 * CR_PREVIEW_SIZE * (y * CR_PREVIEW_SIZE / PHOTO_SIDE);
        for (unsigned x = 0; x < PHOTO_SIDE; ++x) {
            const unsigned char *rgb = row + 3 * (x * CR_PREVIEW_SIZE / PHOTO_SIDE);
            unsigned gray = (77 * rgb[0] + 150 * rgb[1] + 29 * rgb[2]) >> 8;
            bmp_putpixel_fast(vram, 38 + x, 62 + y, COLOR_GRAY(gray * 100 / 255));
        }
    }
}

static void draw(unsigned progress)
{
    if (!display_available())
        return;
    BMP_LOCK(
        bmp_draw_to_idle(1);
        switch (page) {
        case HOME:
            heading("CANOROID", "Arrows: choose   SET: open   MENU: Canon");
            text(FONT_MED, 43, 109, "Make a square print");
            camera_icon(498, 103, 0);
            for (int i = 0; i < 3; ++i) {
                int y = 169 + i * 67;
                int color = selection == i ? COLOR_CYAN : COLOR_GRAY(20);
                bmp_fill(color, 43, y, 396, 50);
                text(FONT(FONT_MED, selection == i ? COLOR_BLACK : COLOR_WHITE, color),
                     61, y + 10, i == 0 ? "Shoot with frame" : i == 1 ? "Monochrome print" : "View photos");
            }
            break;
        case FRAMES:
            heading("Choose a frame", "Left/Right: frame   SET: Live View   PLAY: home");
            for (int i = 0; i < 3; ++i) {
                int x = 58 + i * 213;
                int tint = i == 1 ? COLOR_YELLOW : COLOR_ALMOST_WHITE;
                if (i == frame)
                    bmp_draw_rect(COLOR_CYAN, x - 7, 115, 166, 166);
                bmp_fill(tint, x, 122, 152, 152);
                bmp_fill(COLOR_GRAY(31), x + cr_frames[i].inset * 152 / 1080, 122 + cr_frames[i].top * 152 / 1080, (1080 - 2 * cr_frames[i].inset) * 152 / 1080, cr_frames[i].height * 152 / 1080);
                text(FONT_MED, x - 4, 295, cr_frames[i].name);
            }
            text(FONT_MED, 58, 363, frame_hint ? frame_hint : "1:1   |   Press SET to compose");
            break;
        case DEVELOPING:
            heading("Making your print", "Original JPEG and RAW kept");
            camera_icon(276, 111, (int) progress * 61 / 100);
            text(FONT_MED, 214, 286, waiting_jpeg ? "Waiting for camera JPEG" : progress < 90 ? "Writing framed JPEG" : "Checking saved JPEG");
            bmp_draw_rect(COLOR_ALMOST_WHITE, 135, 343, 450, 17);
            bmp_fill(COLOR_CYAN, 138, 346, (int) progress * 444 / 100, 11);
            break;
        case RESULT:
            heading("Your print", "SET: shoot   Left/Right: compare   PLAY: home");
            render_preview(progress);
            text(FONT_LARGE, 421, 155, compare_original ? "Original" : "Framed");
            text(FONT_MED, 423, 211, print_name);
            text(FONT_SMALL, 424, 245, compare_original ? "Original JPEG" : "1080 x 1080");
            break;
        case GALLERY:
            heading("Saved photos", gallery_loaded ? "Arrows: browse   TRASH: delete   PLAY: home" :
                    gallery_position ? "Arrows: browse   PLAY: home" : "PLAY: home");
            if (gallery_loaded) {
                render_preview(100);
                text(FONT_MED, 420, 205, print_name);
            } else {
                text(FONT_MED, 48, 125, print_name);
                text(FONT_SMALL, 48, 190, error);
            }
            break;
        case DELETE_PRINT:
            heading("Delete framed photo?", "Arrows: choose   SET: confirm   PLAY: cancel");
            text(FONT_MED, 46, 115, print_name);
            text(FONT_MED, 46, 169, "Original JPEG / RAW kept");
            text(FONT_LARGE, 52, 285, delete_selected ? "Cancel" : "> Cancel");
            text(FONT_LARGE, 366, 285, delete_selected ? "> Delete" : "Delete");
            break;
        case FAILED:
            heading("Print failed", "SET: frames   PLAY: home   MENU: Canon");
            text(FONT_MED, 48, 125, source_path);
            for (size_t offset = 0; offset < strlen(error); offset += 24) {
                char line[25];
                size_t count = MIN(24, strlen(error) - offset);
                memcpy(line, error + offset, count);
                line[count] = 0;
                text(FONT_MED, 48, 171 + (offset / 24) * 31, line);
            }
            text(FONT_MED, 48, 323, log_status);
            break;
        }
        bmp_draw_to_idle(0);
        if (display_available()) {
            last_signature = screen_signature(bmp_vram_idle());
            bmp_idle_copy(1, 0);
            last_presented = bmp_vram_real();
            last_display_check = get_ms_clock();
        }
    )
}

static void advance_print(unsigned progress)
{
    if (progress < 100 && !leave_requested) {
        claim_review_screen();
        draw(progress);
    }
}

static unsigned digits(const char *s, int n)
{
    unsigned value = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        value = value * 10 + s[i] - '0';
    }
    return value;
}

/* Eight-character FAT names. Sequence variants keep both copies if Canon
 * reuses a DCF number. Legacy P####.JPG prints remain in the gallery. */
static unsigned photo_key(const char *name)
{
    size_t len = strlen(name);
    if (len == 9 && (name[0] == 'P' || name[0] == 'p') && !strcasecmp(name + 5, ".JPG"))
        return digits(name + 1, 4);
    if (len != 12 || strcasecmp(name + 8, ".JPG")) return 0;
    unsigned folder = digits(name, 3), number = digits(name + 4, 4);
    char variant = name[3];
    if (variant >= 'a' && variant <= 'z') variant -= 'a' - 'A';
    if (folder < 100 || !number || (variant != '_' && (variant < 'A' || variant > 'Z')))
        return 0;
    return (folder * 10000 + number) * 27 + (variant == '_' ? 0 : variant - 'A' + 1);
}

static void release_preview(void)
{
    if (preview) free(preview);
    preview = original_preview = NULL;
    compare_original = gallery_loaded = 0;
}

static void browse_prints(int direction)
{
    release_preview();
    print_name[0] = 0;
    page = GALLERY;
    struct fio_file entry;
    struct fio_dirent *dir = FIO_FindFirstEx(PRINT_DIR "/", &entry);
    if (!dir || IS_ERROR(dir)) {
        snprintf(error, sizeof(error), "Cannot read " PRINT_DIR);
        return;
    }
    unsigned adjacent = 0, wrap = 0;
    char wrap_name[13] = "";
    do {
        if (entry.mode & ATTR_DIRECTORY) continue;
        unsigned k = photo_key(entry.name);
        if (!k) continue;
        if (!wrap || (direction > 0 ? k < wrap : k > wrap)) {
            wrap = k;
            snprintf(wrap_name, sizeof(wrap_name), "%s", entry.name);
        }
        if (direction > 0 ? k > gallery_position && (!adjacent || k < adjacent)
                          : k < gallery_position && k > adjacent) {
            adjacent = k;
            snprintf(print_name, sizeof(print_name), "%s", entry.name);
        }
    } while (FIO_FindNextEx(dir, &entry) == 0);
    FIO_FindClose(dir);
    if (!wrap) {
        gallery_position = 0;
        snprintf(error, sizeof(error), "No saved prints on this card");
        return;
    }
    if (!adjacent) snprintf(print_name, sizeof(print_name), "%s", wrap_name);
    gallery_position = adjacent ? adjacent : wrap;
    char path[48];
    snprintf(path, sizeof(path), PRINT_DIR "/%s", print_name);
    preview = malloc(PREVIEW_BYTES);
    card_read_buffer = fio_malloc(CR_IO_CHUNK);
    if (preview && card_read_buffer)
        gallery_loaded = cr_load_print(path, preview, error);
    else
        snprintf(error, sizeof(error), "Gallery memory allocation failed");
    if (card_read_buffer) fio_free(card_read_buffer);
    card_read_buffer = NULL;
    if (!gallery_loaded) release_preview();
}

static int print_paths(char destination[48], char temporary[48])
{
    const char *dir = strstr(source_path, "/DCIM/");
    const char *base = dir ? strchr(dir + 6, '/') : NULL;
    if (!dir || !base || strlen(base + 1) != 12 ||
        digits(dir + 6, 3) < 100 || !digits(base + 5, 4) || strcasecmp(base + 9, ".JPG")) {
        snprintf(error, sizeof(error), "Invalid DCF JPEG name");
        return 0;
    }
    /* Copy the digits Canon actually wrote. The 600D's formatter does not
     * implement all the conversions provided by the host C library. */
    memcpy(print_name, dir + 6, 3);
    memcpy(print_name + 4, base + 5, 4);
    memcpy(print_name + 8, ".JPG", 5);
    for (int i = 0; i <= 26; i++) {
        print_name[3] = i ? 'A' + i - 1 : '_';
        snprintf(destination, 48, PRINT_DIR "/%s", print_name);
        strcpy(temporary, destination);
        memcpy(temporary + strlen(temporary) - 3, "TMP", 3);
        int jpg = cr_exists(destination), tmp = cr_exists(temporary);
        if (jpg < 0 || tmp < 0) {
            snprintf(error, sizeof(error), "Cannot list " PRINT_DIR);
            return 0;
        }
        if (!jpg && !tmp) return 1;
    }
    snprintf(error, sizeof(error), "All 27 names for this photo already exist");
    return 0;
}

static int write_report(FILE *file, const char *report)
{
    if (!file) return 0;
    int size = strlen(report);
    int ok = cr_write(file, report, size) == size;
    cr_close(file);
    return ok;
}

static void log_failure(void)
{
    char path[40], report[320];
    struct card_info *card = get_shooting_card();
    snprintf(report, sizeof(report), "Canoroid alpha.8\nInput: %s\nFrame: %s\n%s\nDCIM %d / file %d\n\n",
             source_path[0] ? source_path : "(unresolved)", cr_frames[frame].name,
             error, card->folder_number, card->file_number);
    for (int n = 1; n <= 9999; ++n) {
        snprintf(path, sizeof(path), PRINT_DIR "/E%04d.TXT", n);
        int occupied = cr_exists(path);
        if (occupied < 0) break;
        if (occupied) continue;
        if (write_report(cr_open(path, 1), report)) {
            snprintf(log_status, sizeof(log_status), "Log: %s", path);
            return;
        }
        break;
    }
    if (write_report(FIO_CreateFileOrAppend("ML/CANOROID.LOG"), report))
        snprintf(log_status, sizeof(log_status), "Log: ML/CANOROID.LOG");
    else
        snprintf(log_status, sizeof(log_status), "Could not write error log to card");
}

static void make_print(void)
{
    /* The ZIP supplies this directory. Restore it as well on older cards
     * where only autoexec.bin was updated. FIO_CreateDirectory is idempotent. */
    FIO_CreateDirectory(PRINT_DIR);
    char temporary[48], destination[48];
    if (!print_paths(destination, temporary)) {
        page = FAILED;
        log_failure();
        return;
    }
    unsigned bytes = PREVIEW_BYTES;
    unsigned char *pixels = malloc(bytes * 2);
    if (!pixels) {
        snprintf(error, sizeof(error), "Preview memory allocation failed");
        page = FAILED;
        log_failure();
        return;
    }

    card_read_buffer = fio_malloc(CR_IO_CHUNK);
    if (!card_read_buffer) {
        free(pixels);
        snprintf(error, sizeof(error), "Card read buffer allocation failed");
        page = FAILED;
        log_failure();
        return;
    }

    page = DEVELOPING;
    draw(0);
    memset(pixels + bytes, 0, bytes);
    struct cr_frame style = cr_frames[frame];
    style.monochrome = mono;
    int printed = cr_print(source_path, temporary, destination, &style,
                           pixels, pixels + bytes, advance_print, error);
    fio_free(card_read_buffer);
    card_read_buffer = NULL;
    if (!printed) {
        free(pixels);
        log_failure();
        page = FAILED;
        return;
    }

    preview = pixels;
    original_preview = pixels + bytes;
    compare_original = 0;
    gallery_position = photo_key(print_name);
    page = RESULT;
    for (unsigned progress = 12; progress < 100 && !leave_requested; progress += 12) {
        draw(progress);
        msleep(35);
    }
}

/* Called in Canon's GUI task. Only post commands here. File I/O and bitmap
 * redraws belong to the Canoroid task. Return 0 for handled keys. */
int canoroid_handle_key(int button)
{
    if (shooting) {
        if (button == BGMT_TRASH) {
            cancel_shot = 1;
            open_requested = 1;
            return 0;
        }
        if (button == BGMT_MENU || button == BGMT_PLAY)
            cancel_shot = 1;
        return 1;
    }
    if (!active && !opening && !open_requested) {
        if (button == BGMT_TRASH && usable_screen() && !PLAY_MODE &&
            !is_movie_mode() && !get_halfshutter_pressed()) {
            open_requested = 1;
            return 0;
        }
        return 1;
    }

    switch (button) {
    case BGMT_MENU:
        leave_requested = 2;
        return 0;
    case BGMT_PRESS_HALFSHUTTER:
    case BGMT_LV:
        if (page != DEVELOPING) leave_requested = 1;
        return 1;
    case BGMT_TRASH:
        if (opening || open_requested) return 0;
        if (page == GALLERY || page == DELETE_PRINT) post_key(button);
        else if (page != DEVELOPING) leave_requested = 1;
        return 0;
    case BGMT_PRESS_UP:
    case BGMT_PRESS_DOWN:
    case BGMT_PRESS_LEFT:
    case BGMT_PRESS_RIGHT:
    case BGMT_PRESS_SET:
    case BGMT_PLAY:
        if (page != DEVELOPING)
            post_key(button);
        return 0;
    case BGMT_UNPRESS_UP:
    case BGMT_UNPRESS_DOWN:
    case BGMT_UNPRESS_LEFT:
    case BGMT_UNPRESS_RIGHT:
    case BGMT_UNPRESS_SET:
        return 0;
    default:
        return 1;
    }
}

static void guide(void)
{
    get_yuv422_vram();
    int side = MIN(os.x_ex, os.y_ex);
    int x = os.x0 + (os.x_ex - side) / 2;
    int y = os.y0 + (os.y_ex - side) / 2;
    if (side < 100 || x < 0 || y < 0 || x + side > 720 || y + side > 480) return;
    const struct cr_frame *f = &cr_frames[frame];
    BMP_LOCK(
        bmp_draw_rect(COLOR_WHITE, x, y, side-1, side-1);
        bmp_draw_rect(COLOR_WHITE, x + side*f->inset/1080, y + side*f->top/1080,
                      side*(1080-2*f->inset)/1080, side*f->height/1080);
    )
}

static int shooting_mode_ok(void)
{
    return shooting_mode == SHOOTMODE_P || shooting_mode == SHOOTMODE_AV ||
           shooting_mode == SHOOTMODE_TV || shooting_mode == SHOOTMODE_M;
}

/* Canon DCF folders and JPEGs have numbered eight-character basenames.
 * Read actual directory entries, including Canon's folder suffix and prefix. */
struct capture_id { unsigned folder, number; char path[80]; };

static int newest_jpeg(const char *drive, struct capture_id *id)
{
    char root[16], directory[40] = "";
    struct fio_file entry;
    snprintf(root, sizeof(root), "%s:/DCIM", drive);
    memset(id, 0, sizeof(*id));
    struct fio_dirent *d = FIO_FindFirstEx(root, &entry);
    if (!d || IS_ERROR(d)) return 0;
    do {
        unsigned folder = digits(entry.name, 3);
        if ((entry.mode & ATTR_DIRECTORY) && strlen(entry.name) == 8 &&
            folder >= 100 && folder > id->folder) {
            id->folder = folder;
            snprintf(directory, sizeof(directory), "%s/%s", root, entry.name);
        }
    } while (FIO_FindNextEx(d, &entry) == 0);
    FIO_FindClose(d);
    if (!id->folder) return 1;
    d = FIO_FindFirstEx(directory, &entry);
    if (!d || IS_ERROR(d)) return 0;
    do {
        if ((entry.mode & ATTR_DIRECTORY) || strlen(entry.name) != 12 ||
            strcasecmp(entry.name + 8, ".JPG")) continue;
        unsigned number = digits(entry.name + 4, 4);
        if (number > id->number) {
            id->number = number;
            snprintf(id->path, sizeof(id->path), "%s/%s", directory, entry.name);
        }
    } while (FIO_FindNextEx(d, &entry) == 0);
    FIO_FindClose(d);
    return 1;
}

static int newer(const struct capture_id *before, const struct capture_id *after)
{
    return after->number && (after->folder > before->folder ||
           (after->folder == before->folder && after->number > before->number));
}

/* Capture the names which were absent before the shutter. On a single-card
 * 600D the next DCF file can appear before the file-number notification. */
struct expected_files { char path[3][80]; };

static void next_files(struct expected_files *files, const char *drive,
                       const struct card_info *card)
{
    memset(files, 0, sizeof(*files));
    int folder = card->folder_number, number = card->file_number + 1;
    if (number == 10000) { folder++; number = 1; }
    if (folder < 100 || folder > 999 || number < 1 || number > 9999) return;
    const char *suffix = get_dcim_dir_suffix();
    if (!suffix || !*suffix) suffix = "CANON";
    const char *prefix[] = {get_file_prefix(), "IMG_", "_MG_"};
    for (unsigned i = 0; i < 3; i++) {
        if (!prefix[i] || strlen(prefix[i]) != 4) continue;
        snprintf(files->path[i], sizeof(files->path[i]), "%s:/DCIM/%03d%s/%s%04d.JPG",
                 drive, folder, suffix, prefix[i], number);
        if (is_file(files->path[i])) files->path[i][0] = 0;
    }
}

static const char *find_expected(const struct expected_files *files)
{
    for (unsigned i = 0; i < 3; i++)
        if (files->path[i][0] && is_file(files->path[i])) return files->path[i];
    return NULL;
}

static void shoot_with_frame(void)
{
    frame_hint = NULL;
    if (!shooting_mode_ok()) {
        frame_hint = "Select P, Av, Tv or M, then press SET";
        page = FRAMES;
        return;
    }
    release_preview();
    struct card_info *card = get_shooting_card();
    struct capture_id initial, found;
    /* The 600D has a single SD slot. Use the mounted card containing ML
     * for file I/O and the shooting-card property only as a capture signal. */
    const char *drive = get_ml_card()->drive_letter;
    int have_baseline = newest_jpeg(drive, &initial);
    struct expected_files expected;
    next_files(&expected, drive, card);
    source_path[0] = 0;
    retained_page = 0;
    active = 0;
    cancel_shot = 0;
    shooting = 1;
    BMP_LOCK(bmp_fill(COLOR_EMPTY, 0, 0, 720, 480);)
    canon_gui_enable_front_buffer(1);
    SetGUIRequestMode(0);
    for (int i=0; i<50 && CURRENT_GUI_MODE && !ml_shutdown_requested; ++i) msleep(40);
    if (!lv && !CURRENT_GUI_MODE && !ml_shutdown_requested && !cancel_shot)
        fake_simple_button(BGMT_LV);
    for (int i=0; i<75 && !lv && !cancel_shot && !ml_shutdown_requested; ++i) msleep(40);
    if ((!lv || (CURRENT_GUI_MODE && !liveview_display_idle())) && !cancel_shot && !ml_shutdown_requested) {
        shooting = 0;
        active = 1;
        page = FRAMES;
        frame_hint = "Live View did not start. Press SET to retry";
        key_read = key_write;
        retained_page = 1;
        claim_review_screen();
        draw(100);
        return;
    }
    /* Arm after the Live View transition. A lingering Quick Review or
     * the initial delivery of Canon's file counter is not a new exposure. */
    int before = card->file_number;
    int review_was_active = QR_MODE;
    unsigned poll = 0;
    int saw_capture = 0;
    while (!cancel_shot && !ml_shutdown_requested) {
        int review_now = QR_MODE;
        int number_now = card->file_number;
        saw_capture = (review_now && !review_was_active) ||
                      (before > 0 && number_now > 0 && number_now != before);
        review_was_active = review_now;
        if (!saw_capture && !before && number_now > 0) before = number_now;
        if (saw_capture) break;
        if (++poll >= 16) {
            poll = 0;
            const char *path = find_expected(&expected);
            if (path) {
                snprintf(source_path, sizeof(source_path), "%s", path);
                saw_capture = 1;
                break;
            }
            if (have_baseline && newest_jpeg(drive, &found) && newer(&initial, &found)) {
                saw_capture = 1;
                break;
            }
        }
        if (lv && liveview_display_idle() && lv_dispsize == 1) guide();
        msleep(60);
    }
    shooting = 0;
    if (cancel_shot || ml_shutdown_requested) {
        if (open_requested) {
            page = HOME;
            selection = 0;
            key_read = key_write;
        }
        redraw();
        return;
    }
    active = 1;
    key_read = key_write;
    page = DEVELOPING;
    waiting_jpeg = 1;
    uint32_t last = 0;
    int ready = 0;
    unsigned stable = 0;
    const char *pending = "Camera JPEG has not appeared";
    for (int i = 0; saw_capture && i < 250 && !ml_shutdown_requested; i++) {
        claim_review_screen();
        draw(i % 16);
        if (!source_path[0] && i % 3 == 0) {
            const char *path = find_expected(&expected);
            if (path) snprintf(source_path, sizeof(source_path), "%s", path);
            /* Even if enumeration was unavailable, a changed file number
             * identifies this exposure in Canon's current shooting folder. */
            if (!source_path[0] && card->file_number != before && card->file_number > 0 &&
                card->folder_number >= 100 && (!have_baseline ||
                (unsigned)card->folder_number > initial.folder ||
                ((unsigned)card->folder_number == initial.folder &&
                 (unsigned)card->file_number > initial.number))) {
                char candidate[100];
                const char *suffix = get_dcim_dir_suffix();
                if (!suffix || !*suffix) suffix = "CANON";
                const char *prefixes[] = {get_file_prefix(), "IMG_", "_MG_"};
                for (unsigned k = 0; k < 3; k++) {
                    if (!prefixes[k] || !*prefixes[k]) continue;
                    snprintf(candidate, sizeof(candidate), "%s:/DCIM/%03d%s/%s%04d.JPG",
                             drive, card->folder_number, suffix, prefixes[k], card->file_number);
                    if (is_file(candidate)) {
                        snprintf(source_path, sizeof(source_path), "%s", candidate);
                        break;
                    }
                }
            }
            if (!source_path[0] && have_baseline && newest_jpeg(drive, &found) &&
                newer(&initial, &found))
                snprintf(source_path, sizeof(source_path), "%s", found.path);
        }
        uint32_t size = 0;
        if (source_path[0]) {
            int stat_ok = FIO_GetFileSize(source_path, &size) == 0;
            stable = stat_ok && size > 2 && size == last ? stable + 1 : 0;
            pending = !stat_ok ? "Cannot read camera JPEG size" : "Waiting for complete camera JPEG";
            if (stable >= 2) {
                FILE *f = FIO_OpenFile(source_path, O_RDONLY | O_SYNC);
                if (f) {
                    unsigned char tail[2];
                    int tail_ok = FIO_SeekSkipFile(f, size-2, SEEK_SET) >= 0 &&
                                  FIO_ReadFile(f, tail, 2) == 2;
                    FIO_CloseFile(f);
                    /* Camera-ready describes the NEXT exposure. Do not block
                     * a sealed JPEG while Canon is still in Quick Review.
                     * Some JPEG writers also pad after EOI. In that case let
                     * the full decoder validate the stable file once idle. */
                    ready = tail_ok && ((tail[0] == 255 && tail[1] == 217) ||
                            (stable >= 10 && job_state_ready_to_take_pic()));
                    pending = tail_ok ? "Camera JPEG incomplete or busy" : "Cannot read camera JPEG end";
                } else {
                    pending = "Cannot open camera JPEG";
                }
            }
            if (ready) break;
        }
        last = size;
        msleep(100);
    }
    waiting_jpeg = 0;
    if (ml_shutdown_requested) return;
    if (ready) make_print();
    else {
        snprintf(error, sizeof(error), "%s", source_path[0] ? pending :
                 !have_baseline ? "Cannot identify shot in camera folder" : "No new JPEG in DCIM");
        page = FAILED;
        if (saw_capture) log_failure();
    }
    retained_page = 1;
    claim_review_screen();
    draw(100);
}

static void handle_key(int button)
{
    if (page == DELETE_PRINT) {
        if (button == BGMT_PRESS_LEFT) delete_selected = 0;
        if (button == BGMT_PRESS_RIGHT) delete_selected = 1;
        if (button == BGMT_PLAY || button == BGMT_TRASH) page = GALLERY;
        if (button == BGMT_PRESS_SET) {
            page = GALLERY;
            if (display_available() && delete_selected && gallery_loaded && gallery_position &&
                photo_key(print_name) == gallery_position) {
                char path[48];
                snprintf(path, sizeof(path), PRINT_DIR "/%s", print_name);
                if (is_file(path) && cr_remove(path)) {
                    browse_prints(1);
                } else {
                    release_preview();
                    snprintf(error, sizeof(error), "Could not delete framed JPEG");
                }
            }
        }
    } else if (button == BGMT_PLAY) {
        /* Home needs a stable menu dialog and its button events even
         * when the preceding print was shown over Canon Quick Review. */
        if (retained_page) open_requested = 1;
        release_preview();
        selection = 0;
        frame_hint = NULL;
        page = HOME;
    } else if (page == HOME) {
        int choices = 3;
        if (button == BGMT_PRESS_UP) selection = (selection + choices - 1) % choices;
        if (button == BGMT_PRESS_DOWN) selection = (selection + 1) % choices;
        if (button == BGMT_PRESS_SET && selection < 2) { mono = selection; page = FRAMES; }
        if (button == BGMT_PRESS_SET && selection == 2) {
            gallery_position = 0;
            browse_prints(-1);
        }
    } else if (page == FRAMES) {
        if (button == BGMT_PRESS_LEFT)
            frame = (frame + 2) % 3;
        if (button == BGMT_PRESS_RIGHT)
            frame = (frame + 1) % 3;
        if (button == BGMT_PRESS_SET)
            shoot_with_frame();
    } else if (page == GALLERY) {
        if (button == BGMT_TRASH && gallery_loaded) {
            delete_selected = 0;
            page = DELETE_PRINT;
        }
        if (button == BGMT_PRESS_LEFT || button == BGMT_PRESS_RIGHT)
            browse_prints(button == BGMT_PRESS_LEFT ? -1 : 1);
    } else if (page == RESULT) {
        if (button == BGMT_PRESS_LEFT || button == BGMT_PRESS_RIGHT) compare_original = !compare_original;
        if (button == BGMT_PRESS_SET) shoot_with_frame();
    } else if (button == BGMT_PRESS_SET) {
        page = page == FAILED ? FRAMES : HOME;
    }
}

static int can_open(void)
{
    return usable_screen() && !get_halfshutter_pressed() &&
           job_state_ready_to_take_pic();
}

static void canoroid_task(void *unused)
{
    (void) unused;
    extern int ml_started;
    extern int ml_gui_initialized;
    while (!ml_started || !ml_gui_initialized)
        msleep(100);
    FILE *log = FIO_CreateFile(PRINT_DIR "/STARTUP.TXT");
    if (log) {
        const char message[] = "Canoroid 0.1.0-alpha.8: task and Canon GUI ready.\n";
        FIO_WriteFile(log, message, sizeof(message) - 1);
        FIO_CloseFile(log);
    }
    open_requested = 1;
    unsigned retries = 0;
    for (;;) {
        if (open_requested && (active || can_open()) && !leave_requested) {
            open_requested = 0;
            opening = 1;
            active = 0;
            retained_page = 0;
            if (canon_gui_front_buffer_disabled()) canon_gui_enable_front_buffer(0);
            gui_mode = GUIMODE_ML_MENU;
            SetGUIRequestMode(gui_mode);
            for (int i = 0; i < 100 && CURRENT_GUI_MODE != gui_mode &&
                 !ml_shutdown_requested && !leave_requested; ++i) msleep(10);
            msleep(50);
            if (CURRENT_GUI_MODE == gui_mode && DISPLAY_IS_ON && !ml_shutdown_requested &&
                !get_halfshutter_pressed() && !leave_requested) {
                canon_gui_disable_front_buffer();
                bmp_on();
                active = 1;
                retries = 0;
                draw(100);
            } else if (usable_screen() && !ml_shutdown_requested && !leave_requested && retries++ < 2) {
                open_requested = 1;
            } else {
                leave_requested = 1;
            }
            opening = 0;
        }
        if (active && !leave_requested && !ml_shutdown_requested && DISPLAY_IS_ON) {
            if (!retained_page && CURRENT_GUI_MODE != gui_mode) {
                /* A closed dialog leaves the last bitmap visible but can
                 * stop producing menu key events. Reopen the dialog itself. */
                if (usable_screen()) open_requested = 1;
                else leave_requested = 1;
            } else if (usable_screen() && (unsigned)get_ms_clock() - last_display_check >= 250) {
                last_display_check = get_ms_clock();
                uint8_t *front = bmp_vram_real();
                if (front && (!display_available() || last_presented != front ||
                              screen_signature(front) != last_signature)) {
                    claim_review_screen();
                    draw(100);
                }
            }
        }
        if (leave_requested) {
            int to_menu = leave_requested == 2;
            active = 0;
            open_requested = 0;
            key_read = key_write;
            if (!ml_shutdown_requested) {
                if (canon_gui_front_buffer_disabled()) {
                    BMP_LOCK(bmp_fill(COLOR_EMPTY, 0, 0, 720, 480);)
                    canon_gui_enable_front_buffer(0);
                }
                if (to_menu || CURRENT_GUI_MODE == gui_mode)
                    SetGUIRequestMode(to_menu ? GUIMODE_MENU : 0);
                redraw();
            }
            release_preview();
            retained_page = 0;
            page = HOME;
            selection = 0;
            retries = 0;
            leave_requested = 0;
        }
        /* Framebuffer contention must not stop key dispatch once Canon
         * has installed our menu dialog. */
        if (active && !open_requested && DISPLAY_IS_ON && !ml_shutdown_requested &&
            CURRENT_GUI_MODE == gui_mode && key_read != key_write) {
            int button = key_queue[key_read];
            key_read = (key_read + 1) % KEY_SLOTS;
            handle_key(button);
            if (active && !open_requested && !leave_requested) draw(100);
        }
        msleep(40);
    }
}

TASK_CREATE("canoroid", canoroid_task, 0, 0x1e, 0x8000);
