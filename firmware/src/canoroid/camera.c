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

enum page { HOME, FRAMES, DEVELOPING, RESULT, FAILED };

static volatile int active;
static volatile int open_requested;
static volatile int leave_requested;
static volatile int key;
static int selection;
static int frame;
static int gui_mode;
static volatile enum page page;
static unsigned char *preview;
static unsigned char *card_read_buffer;
static char error[96];
static unsigned print_number;
static int retained_page;
static int waiting_jpeg;
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
    return write ? FIO_CreateFile(path) : FIO_OpenFile(path, O_RDONLY | O_SYNC);
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
    return is_file(path) || is_dir(path);
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

static void render_preview(int height)
{
    uint8_t *vram = bmp_vram();
    if (!vram)
        return;
    for (int y = 0; y < height; ++y) {
        const unsigned char *rgb = (compare_original ? original_preview : preview) + y * CR_PREVIEW_SIZE * 3;
        for (int x = 0; x < CR_PREVIEW_SIZE; ++x, rgb += 3) {
            unsigned gray = (77 * rgb[0] + 150 * rgb[1] + 29 * rgb[2]) >> 8;
            bmp_putpixel_fast(vram, 95 + x, 111 + y, COLOR_GRAY(gray * 100 / 255));
        }
    }
}

static void draw(unsigned progress)
{
    if (!display_available())
        return;
    BMP_LOCK(
        switch (page) {
        case HOME:
            heading("CANOROID", "Arrows: choose   SET: open   MENU: Canon");
            text(FONT_MED, 43, 109, "Make a square print");
            camera_icon(498, 103, 0);
            for (int i = 0; i < (preview ? 3 : 2); ++i) {
                int y = 169 + i * 67;
                int color = selection == i ? COLOR_CYAN : COLOR_GRAY(20);
                bmp_fill(color, 43, y, 396, 50);
                text(FONT(FONT_MED, selection == i ? COLOR_BLACK : COLOR_WHITE, color),
                     61, y + 10, i == 0 ? "Shoot with frame" : i == 1 ? "Monochrome print" : "View last print");
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
            render_preview((int) progress * CR_PREVIEW_SIZE / 100);
            text(FONT_LARGE, 421, 155, compare_original ? "Original" : "Framed");
            bmp_printf(FONT_MED, 423, 211, "P%04u.JPG", print_number);
            text(FONT_SMALL, 424, 245, compare_original ? "Original JPEG" : "1080 x 1080");
            break;
        case FAILED:
            heading("Print failed", "SET: frames   PLAY: home   MENU: Canon");
            text(FONT_MED, 48, 125, source_path);
            /* Fonts on this platform have a fixed maximum line width. */
            {
                char line[49];
                snprintf(line, sizeof(line), "%.48s", error);
                text(FONT_SMALL, 48, 191, line);
                if (strlen(error) > 48)
                    text(FONT_SMALL, 48, 222, error + 48);
            }
            break;
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

static unsigned reserve_number(void)
{
    for (unsigned n = 1; n <= 9999; ++n) {
        char path[40];
        snprintf(path, sizeof(path), PRINT_DIR "/P%04u.JPG", n);
        if (cr_exists(path))
            continue;
        snprintf(path, sizeof(path), PRINT_DIR "/P%04u.TMP", n);
        if (cr_exists(path))
            continue;
        snprintf(path, sizeof(path), PRINT_DIR "/E%04u.TXT", n);
        if (!cr_exists(path))
            return n;
    }
    return 0;
}

static void log_failure(unsigned number)
{
    char path[40];
    snprintf(path, sizeof(path), PRINT_DIR "/E%04u.TXT", number);
    if (cr_exists(path))
        return;
    void *file = cr_open(path, 1);
    if (file) {
        char report[180];
        snprintf(report, sizeof(report), "%s\n%s\n%s\n", source_path,
                 cr_frames[frame].name, error);
        cr_write(file, report, strlen(report));
        cr_close(file);
    }
}

static void make_print(void)
{
    if (!is_dir(PRINT_DIR) && FIO_CreateDirectory(PRINT_DIR) != 0) {
        snprintf(error, sizeof(error), "Create Canoroid directory failed");
        page = FAILED;
        return;
    }
    unsigned number = reserve_number();
    if (!number) {
        snprintf(error, sizeof(error), "No available print number");
        page = FAILED;
        return;
    }
    unsigned bytes = CR_PREVIEW_SIZE * CR_PREVIEW_SIZE * 3;
    unsigned char *pixels = malloc(bytes * 2);
    if (!pixels) {
        snprintf(error, sizeof(error), "Preview memory allocation failed");
        page = FAILED;
        log_failure(number);
        return;
    }

    card_read_buffer = fio_malloc(CR_IO_CHUNK);
    if (!card_read_buffer) {
        free(pixels);
        snprintf(error, sizeof(error), "Card read buffer allocation failed");
        page = FAILED;
        log_failure(number);
        return;
    }

    char temporary[40];
    char destination[40];
    snprintf(temporary, sizeof(temporary), PRINT_DIR "/P%04u.TMP", number);
    snprintf(destination, sizeof(destination), PRINT_DIR "/P%04u.JPG", number);
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
        log_failure(number);
        page = FAILED;
        return;
    }

    if (preview)
        free(preview);
    preview = pixels;
    original_preview = pixels + bytes;
    compare_original = 0;
    print_number = number;
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
    if (!active) {
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
        leave_requested = 1;
        return 0;
    case BGMT_PRESS_UP:
    case BGMT_PRESS_DOWN:
    case BGMT_PRESS_LEFT:
    case BGMT_PRESS_RIGHT:
    case BGMT_PRESS_SET:
    case BGMT_PLAY:
        if (page != DEVELOPING)
            key = button;
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

static unsigned digits(const char *s, int n)
{
    unsigned value = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        value = value * 10 + s[i] - '0';
    }
    return value;
}

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

static void shoot_with_frame(void)
{
    frame_hint = NULL;
    if (!shooting_mode_ok()) {
        frame_hint = "Select P, Av, Tv or M, then press SET";
        page = FRAMES;
        return;
    }
    struct card_info *card = get_shooting_card();
    struct capture_id initial, found;
    /* The 600D has a single SD slot. Use the mounted card containing ML
     * for file I/O and the shooting-card property only as a capture signal. */
    const char *drive = get_ml_card()->drive_letter;
    int have_baseline = newest_jpeg(drive, &initial);
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
    if (!lv && !cancel_shot && !ml_shutdown_requested) {
        shooting = 0;
        active = 1;
        page = FRAMES;
        frame_hint = "Live View did not start. Press SET to retry";
        key = 0;
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
            if (have_baseline && newest_jpeg(drive, &found) && newer(&initial, &found)) {
                saw_capture = 1;
                break;
            }
        }
        if (lv && liveview_display_idle() && lv_dispsize == 1) guide();
        msleep(60);
    }
    shooting = 0;
    if (cancel_shot || ml_shutdown_requested) { redraw(); return; }
    active = 1;
    key = 0;
    page = DEVELOPING;
    waiting_jpeg = 1;
    uint32_t last = 0;
    int ready = 0;
    for (int i = 0; saw_capture && i < 250 && !ml_shutdown_requested; i++) {
        claim_review_screen();
        draw(i % 16);
        if (!source_path[0] && i % 3 == 0) {
            /* Even if enumeration was unavailable, a changed file number
             * identifies this exposure in Canon's current shooting folder. */
            if (card->file_number != before && card->file_number > 0 &&
                card->folder_number >= 100) {
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
        if (source_path[0] && !FIO_GetFileSize(source_path, &size) && size > 2 &&
            size == last && job_state_ready_to_take_pic()) {
            FILE *f = FIO_OpenFile(source_path, O_RDONLY | O_SYNC);
            if (f) {
                unsigned char tail[2];
                if (FIO_SeekSkipFile(f, size-2, SEEK_SET) >= 0 &&
                    FIO_ReadFile(f, tail, 2) == 2 && tail[0] == 255 && tail[1] == 217)
                    ready = 1;
                FIO_CloseFile(f);
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
        snprintf(error, sizeof(error), "%s", source_path[0] ? "Camera JPEG incomplete or busy" :
                 !have_baseline ? "Cannot identify shot in camera folder" : "No new JPEG in DCIM");
        page = FAILED;
        if (saw_capture) {
            unsigned number = reserve_number();
            if (number) log_failure(number);
        }
    }
    retained_page = 1;
    claim_review_screen();
    draw(100);
}

static void handle_key(int button)
{
    if (button == BGMT_PLAY) {
        retained_page = 0;
        page = HOME;
    } else if (page == HOME) {
        int choices = preview ? 3 : 2;
        if (button == BGMT_PRESS_UP) selection = (selection + choices - 1) % choices;
        if (button == BGMT_PRESS_DOWN) selection = (selection + 1) % choices;
        if (button == BGMT_PRESS_SET && selection < 2) { mono = selection; page = FRAMES; }
        if (button == BGMT_PRESS_SET && selection == 2 && preview)
            page = RESULT;
    } else if (page == FRAMES) {
        if (button == BGMT_PRESS_LEFT)
            frame = (frame + 2) % 3;
        if (button == BGMT_PRESS_RIGHT)
            frame = (frame + 1) % 3;
        if (button == BGMT_PRESS_SET)
            shoot_with_frame();
    } else if (page == RESULT) {
        if (button == BGMT_PRESS_LEFT || button == BGMT_PRESS_RIGHT) compare_original = !compare_original;
        if (button == BGMT_PRESS_SET) shoot_with_frame();
    } else if (button == BGMT_PRESS_SET) {
        page = page == FAILED ? FRAMES : HOME;
    }
}

static int can_open(void)
{
    return usable_screen() && (MENU_MODE || CURRENT_GUI_MODE == 0 || QR_MODE) && !get_halfshutter_pressed() &&
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
        const char message[] = "Canoroid 0.1.0-alpha.5: task and Canon GUI ready.\n";
        FIO_WriteFile(log, message, sizeof(message) - 1);
        FIO_CloseFile(log);
    }
    open_requested = 1;
    unsigned retries = 0;
    unsigned ticks = 0;
    for (;;) {
        if (!active && open_requested && can_open()) {
            open_requested = 0;
            leave_requested = 0;
            key = 0;
            gui_mode = GUIMODE_ML_MENU;
            SetGUIRequestMode(gui_mode);
            for (int i = 0; i < 100 && CURRENT_GUI_MODE != gui_mode && !ml_shutdown_requested; ++i)
                msleep(10);
            if (CURRENT_GUI_MODE == gui_mode && DISPLAY_IS_ON &&
                !ml_shutdown_requested && !get_halfshutter_pressed()) {
                msleep(50);
                if (CURRENT_GUI_MODE != gui_mode) continue;
                canon_gui_disable_front_buffer();
                bmp_on();
                if (!retained_page) page = HOME;
                active = 1;
                retries = 0;
                draw(100);
            } else if (CURRENT_GUI_MODE == 0 && retries++ < 2) {
                msleep(250);
                open_requested = 1;
            }
        }
        if (active && !leave_requested && DISPLAY_IS_ON && !ml_shutdown_requested &&
            CURRENT_GUI_MODE == gui_mode && !get_halfshutter_pressed()) {
            if (!canon_gui_front_buffer_disabled())
                canon_gui_disable_front_buffer();
            if (++ticks >= 10) {
                ticks = 0;
                draw(100);
            }
        }
        if (active && retained_page && !leave_requested && usable_screen() && !display_available()) {
            claim_review_screen();
            draw(100);
        }
        if (active && (leave_requested || (!display_available() && !retained_page))) {
            int to_menu = leave_requested == 2;
            int still_owns_mode = CURRENT_GUI_MODE == gui_mode;
            if (still_owns_mode && !ml_shutdown_requested) {
                BMP_LOCK(bmp_fill(COLOR_EMPTY, 0, 0, 720, 480);)
                canon_gui_enable_front_buffer(1);
                SetGUIRequestMode(to_menu ? GUIMODE_MENU : 0);
            } else if (canon_gui_front_buffer_disabled()) {
                canon_gui_enable_front_buffer(0);
            }
            active = 0;
            retained_page = 0;
        }
        if (active && key) {
            int button = key;
            key = 0;
            handle_key(button);
            if (!leave_requested)
                draw(100);
        }
        msleep(40);
    }
}

TASK_CREATE("canoroid", canoroid_task, 0, 0x1e, 0x8000);
