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
    text(FONT_SMALL, 38, 438, controls);
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
            heading("CANOROID", "Arrows  Choose     SET  Open     MENU  Canon");
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
            heading("Choose a frame", "Left / Right  Browse     SET  Live View     PLAY  Home");
            for (int i = 0; i < 3; ++i) {
                int x = 58 + i * 213;
                int tint = i == 1 ? COLOR_YELLOW : COLOR_ALMOST_WHITE;
                if (i == frame)
                    bmp_draw_rect(COLOR_CYAN, x - 7, 115, 166, 166);
                bmp_fill(tint, x, 122, 152, 152);
                bmp_fill(COLOR_GRAY(31), x + cr_frames[i].inset * 152 / 1080, 122 + cr_frames[i].top * 152 / 1080, (1080 - 2 * cr_frames[i].inset) * 152 / 1080, cr_frames[i].height * 152 / 1080);
                text(FONT_MED, x - 4, 295, cr_frames[i].name);
            }
            text(FONT_SMALL, 58, 363, "1:1   |   Press SET to compose");
            break;
        case DEVELOPING:
            heading("Making your print", "Original JPEG and RAW kept");
            camera_icon(276, 111, (int) progress * 61 / 100);
            text(FONT_MED, 214, 286, progress < 90 ? "Writing framed JPEG" : "Checking saved JPEG");
            bmp_draw_rect(COLOR_ALMOST_WHITE, 135, 343, 450, 17);
            bmp_fill(COLOR_CYAN, 138, 346, (int) progress * 444 / 100, 11);
            break;
        case RESULT:
            heading("Your print", "SET  Shoot again    Left/Right  Compare    PLAY  Home");
            render_preview((int) progress * CR_PREVIEW_SIZE / 100);
            text(FONT_LARGE, 421, 155, compare_original ? "Original" : "Framed");
            bmp_printf(FONT_MED, 423, 211, "P%04u.JPG", print_number);
            text(FONT_SMALL, 424, 245, compare_original ? "Original JPEG" : "1080 x 1080");
            break;
        case FAILED:
            heading("Print failed", "SET  Choose frame     PLAY  Home     MENU  Canon");
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
    if (progress < 100 && !leave_requested)
        draw(progress);
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
        if (button == BGMT_MENU || button == BGMT_PLAY || button == BGMT_TRASH)
            cancel_shot = 1;
        return 1;
    }
    if (!active) {
        if (button == BGMT_TRASH && CURRENT_GUI_MODE == 0 && !lv &&
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
        leave_requested = 1;
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

static void shoot_with_frame(void)
{
    if (!shooting_mode_ok()) {
        snprintf(error, sizeof(error), "Choose P, Av, Tv or M");
        page = FAILED;
        return;
    }
    struct card_info *card = get_shooting_card();
    int before = card->file_number;
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
    int entered = lv;
    unsigned idle = 0;
    while (entered && !cancel_shot && !ml_shutdown_requested && card->file_number == before) {
        if (liveview_display_idle() && lv) {
            idle = 0;
            if (lv_dispsize == 1) guide();
        } else if (!lens_info.job_state && !get_halfshutter_pressed() && ++idle > 150) {
            break;
        }
        msleep(60);
    }
    int shot = card->file_number;
    int ready = 0;
    if (shot != before && !cancel_shot && !ml_shutdown_requested) {
        char dir[64];
        snprintf(dir,sizeof(dir),"%s",get_dcim_dir());
        uint32_t last = 0;
        for (int i=0; i<200 && !cancel_shot && !ml_shutdown_requested; ++i) {
            uint32_t size = 0;
            snprintf(source_path,sizeof(source_path),"%s/%s%04d.JPG",dir,get_file_prefix(),shot);
            if (FIO_GetFileSize(source_path,&size)) {
                snprintf(source_path,sizeof(source_path),"%s/_MG_%04d.JPG",dir,shot);
                FIO_GetFileSize(source_path,&size);
            }
            if (card->file_number != shot) break;
            if (size && size == last && !lens_info.job_state) { ready = 1; break; }
            last = size;
            msleep(100);
        }
    }
    shooting = 0;
    if (cancel_shot || ml_shutdown_requested) { redraw(); return; }
    gui_mode = GUIMODE_ML_MENU;
    SetGUIRequestMode(gui_mode);
    for (int i=0; i<50 && CURRENT_GUI_MODE != gui_mode; ++i) msleep(40);
    if (CURRENT_GUI_MODE != gui_mode || ml_shutdown_requested) return;
    canon_gui_disable_front_buffer();
    bmp_on();
    active = 1;
    key = 0;
    if (ready) make_print();
    else {
        snprintf(error,sizeof(error),"%s", !entered ? "Live View did not start" :
                 shot == before ? "Capture ended without a new shot" : "JPEG not ready: select JPEG or RAW+JPEG");
        page = FAILED;
        if (shot != before) {
            unsigned number = reserve_number();
            if (number) log_failure(number);
        }
    }
}

static void handle_key(int button)
{
    if (button == BGMT_PLAY) {
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
    return DISPLAY_IS_ON && !ml_shutdown_requested && !lv &&
           !is_movie_mode() && CURRENT_GUI_MODE == 0 &&
           gui_state == GUISTATE_IDLE && !get_halfshutter_pressed() &&
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
        const char message[] = "Canoroid 0.1.0-alpha.3: task and Canon GUI ready.\n";
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
                page = HOME;
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
        if (active && (leave_requested || !display_available())) {
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
