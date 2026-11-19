/* SPDX-License-Identifier: GPL-2.0-only */
#include "dryos.h"
#include "bmp.h"
#include "fio-ml.h"
#include "gui.h"
#include "gui-common.h"
#include "lens.h"
#include "propvalues.h"
#include "lvinfo.h"
#include "task_utils.h"
#include "zebra.h"
#include "print.h"
#include "ui.h"
#include "fileprefix.h"
#include "vram.h"

#define PRINT_DIR "DCIM/CANOROID"
#define LEGACY_DIR "ML/CANOROID"
static const char *const print_dirs[] = {LEGACY_DIR, PRINT_DIR};
static char source_path[100];
static unsigned char *original_preview;
static int compare_original;
static int mono, tone = 1;
static const char *const home_labels[] = {
    "Faded Film", "Vivid Pop", "Redscale 100", "Noir 3200", "View Photos"
};
static volatile int shooting, cancel_shot;

enum page { HOME, COVER, INFO_COVER, FRAMES, DEVELOPING, RESULT, GALLERY, DELETE_PRINT, FAILED };

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
static int photo_index, photo_total;
static uint32_t print_bytes, source_bytes;
static unsigned print_elapsed_ms;
static char photo_date[20];
static int has_capture_date;
static int photo_palette_on;
static uint32_t palette_probe;
#define PREVIEW_BYTES (CR_PREVIEW_SIZE * CR_PREVIEW_SIZE * 3)
static int retained_page;
static int waiting_jpeg;
/* The 600D briefly shows Quick Review and switches the display back to LV.
 * Keep processing files across that transition; present only on a stable bank. */
static int awaiting_display, reveal_pending;
static uint8_t *candidate_buffer;
static int candidate_mode, candidate_review;
static unsigned candidate_since, cover_since;
static int cover_presented;
static char log_status[64];
static const char *frame_hint;

/* Queried by the ML overlay tasks before writing to the shared LCD bitmap. */
int canoroid_owns_display(void)
{
    return active || opening || (shooting && QR_MODE);
}

static int usable_screen(void)
{
    return DISPLAY_IS_ON && !ml_shutdown_requested && !is_movie_mode() &&
           (CURRENT_GUI_MODE == 0 || CURRENT_GUI_MODE == GUIMODE_ML_MENU ||
            MENU_MODE || QR_MODE);
}

/* The 600D can still be showing its Live View quick controls when the
 * user requests Home. They may be dismissed before taking the still menu. */
static int can_open(void)
{
    if (!DISPLAY_IS_ON || ml_shutdown_requested || is_movie_mode() ||
        PLAY_MODE || get_halfshutter_pressed()) return 0;
    return usable_screen() || (lv && (gui_state == GUISTATE_QMENU ||
           CURRENT_GUI_MODE == GUIMODE_Q_UNAVI || CURRENT_GUI_MODE == GUIMODE_WB ||
           CURRENT_GUI_MODE == GUIMODE_DRIVE_MODE || CURRENT_GUI_MODE == GUIMODE_FOCUS_MODE));
}

/* Quick Review and Live View can exchange GUI modes during a single print. */
static void claim_review_screen(void)
{
    if ((page == DEVELOPING || page == RESULT) && QR_MODE && image_review_time != 0xff) {
        awaiting_display = 1;
        candidate_buffer = NULL;
        return;
    }
    if (awaiting_display) {
        int review = QR_MODE;
        int idle = lv ? liveview_display_idle() : gui_state == GUISTATE_IDLE;
        if (!usable_screen() || (review ? image_review_time != 0xff : !idle)) {
            candidate_buffer = NULL;
            return;
        }
        uint8_t *front = bmp_vram_real();
        if (!front) {
            candidate_buffer = NULL;
            return;
        }
        if (candidate_buffer != front || candidate_mode != CURRENT_GUI_MODE ||
            candidate_review != review) {
            candidate_buffer = front;
            candidate_mode = CURRENT_GUI_MODE;
            candidate_review = review;
            candidate_since = get_ms_clock();
            return;
        }
        if ((unsigned)get_ms_clock() - candidate_since < 200) return;
        awaiting_display = 0;
    }
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

int cr_seek(void *file, unsigned offset)
{
    return FIO_SeekSkipFile(file, offset, SEEK_SET) >= 0;
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
    const char *directory = PRINT_DIR "/";
    if (strncmp(path, directory, strlen(directory))) {
        directory = LEGACY_DIR "/";
        if (strncmp(path, directory, strlen(directory))) return -1;
    }
    if (is_file(path)) return 1;
    const char *name = path + strlen(directory);
    struct fio_file entry;
    struct fio_dirent *dir = FIO_FindFirstEx(directory, &entry);
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
    return !awaiting_display && DISPLAY_IS_ON && !ml_shutdown_requested &&
           CURRENT_GUI_MODE == gui_mode && canon_gui_front_buffer_disabled();
}

static void text(int font, int x, int y, const char *label)
{
    if (font == FONT_MED || font == FONT_MED_LARGE || font == FONT_SMALL || font == FONT_LARGE)
        font = FONT(font == FONT_SMALL ? (int)FONT_MED_LARGE : font, UI_TEXT, UI_BG);
    bmp_printf(font, x, y, "%s", label);
}

static void heading(const char *title, const char *controls)
{
    bmp_fill(UI_BG, 0, 0, 720, 480);
    bmp_fill(UI_PANEL, 0, 0, 720, 56);
    bmp_fill(UI_BRAND, UI_MARGIN, 12, 6, 32);
    text(FONT(FONT_LARGE, UI_TEXT, UI_PANEL), 44, 11, title);
    if (!controls) return;
    bmp_fill(COLOR_GRAY(55), 36, 426, 648, 2);
    text(FONT_MED_LARGE, 38, 434, controls);
}

/* Inset spans give an eight-pixel radius on the indexed bitmap layer. */
static void rounded(int c, int x, int y, int w, int h)
{
    static const unsigned char inset[] = {6,4,3,2,1,1};
    bmp_fill(c, x, y+6, w, h-12);
    for (int i=0; i<6; ++i) {
        int d=inset[i];
        bmp_fill(c,x+d,y+i,w-2*d,1);
        bmp_fill(c,x+d,y+h-1-i,w-2*d,1);
    }
}

static void button_face(int x, int y, int w, int h, int fill, int ink, const char *label)
{
    rounded(UI_EDGE,x,y,w,h);
    rounded(fill,x+2,y+2,w-4,h-4);
    text(FONT(FONT_MED_LARGE,ink,fill) | FONT_ALIGN_CENTER,
         x+w/2,y+(h-28)/2,label);
}

/* Draw the key symbol separately so it reads as a physical control. */
enum footer_key { KEY_VERTICAL, KEY_HORIZONTAL, KEY_SET, KEY_MENU, KEY_PLAY, KEY_TRASH };
static void arrow(int x, int y, int radius, int horizontal, int direction, int ink)
{
    for (int i=0; i<=radius; ++i) {
        int offset=direction*(radius/2-i);
        if (horizontal) bmp_fill(ink,x+offset,y-i,1,2*i+1);
        else bmp_fill(ink,x-i,y+offset,2*i+1,1);
    }
}

static void key_action(int x, enum footer_key key, const char *label)
{
    int edge=key==KEY_SET ? UI_CYAN : UI_MUTED;
    rounded(edge,x,428,216,46);
    rounded(UI_PANEL,x+2,430,212,42);
    rounded(edge,x+10,434,70,34);
    int cx=x+45;
    if (key==KEY_VERTICAL) {
        arrow(cx,443,7,0,-1,UI_BG);
        arrow(cx,458,7,0,1,UI_BG);
    } else if (key==KEY_HORIZONTAL) {
        arrow(cx-10,451,8,1,-1,UI_BG);
        arrow(cx+10,451,8,1,1,UI_BG);
    } else if (key==KEY_TRASH) {
        bmp_draw_rect(UI_BG,cx-8,447,17,16);
        bmp_fill(UI_BG,cx-11,443,23,2);
        bmp_fill(UI_BG,cx-4,440,9,2);
        bmp_fill(UI_BG,cx-3,450,2,10);
        bmp_fill(UI_BG,cx+2,450,2,10);
    } else if (key==KEY_PLAY) {
        arrow(cx,451,10,1,1,UI_BG);
    } else {
        text(FONT(FONT_MED,UI_BG,edge) | FONT_ALIGN_CENTER,cx,439,key==KEY_SET ? "SET" : "MENU");
    }
    text(FONT(FONT_MED_LARGE,UI_TEXT,UI_PANEL),x+89,437,label);
}

static void photo_actions(int gallery)
{
    if (!gallery || gallery_position)
        key_action(24,KEY_HORIZONTAL,gallery ? "Browse" : "Compare");
    if (!gallery || gallery_loaded)
        key_action(252,gallery ? KEY_TRASH : KEY_SET,gallery ? "Delete" : "Shoot");
    key_action(480,KEY_PLAY,"Home");
}

/* Rasterize the reference icon at either LCD size from one set of coordinates. */
static void disc(int color, int cx, int cy, int radius)
{
    int width=radius;
    for (int dy=0; dy<=radius; ++dy) {
        while (width*width+dy*dy > radius*radius) --width;
        bmp_fill(color,cx-width,cy+dy,2*width+1,1);
        if (dy) bmp_fill(color,cx-width,cy-dy,2*width+1,1);
    }
}

#define ICON_RGB(r,g,b) (80 + 28*(((r)*5+127)/255) + 4*(((g)*6+127)/255) + (((b)*3+127)/255))
static void camera_icon(int x, int y, int size)
{
    const int dark=COLOR_ALMOST_BLACK, glass=COLOR_GRAY(56);
    static const int stripe[] = {ICON_RGB(232,64,42),ICON_RGB(242,140,40),
        ICON_RGB(246,201,69),ICON_RGB(61,176,107),ICON_RGB(45,127,214)};
#define S(n) ((n)*size/160)
    rounded(dark,x,y,size,S(124));
    rounded(ICON_RGB(242,232,207),x+S(3),y+S(3),S(154),S(91));
    for (int i=0;i<5;++i)
        bmp_fill(stripe[i],x+S(66+6*i),y+S(3),S(72+6*i)-S(66+6*i),S(91));
    rounded(dark,x+S(13),y+S(14),S(24),S(24));
    rounded(dark,x+S(117),y+S(11),S(35),S(32));
    rounded(glass,x+S(120),y+S(14),S(29),S(26));
    disc(dark,x+S(135),y+S(27),S(6));
    disc(dark,x+S(80),y+S(49),S(36));
    disc(COLOR_GRAY(37),x+S(80),y+S(49),S(29));
    disc(dark,x+S(80),y+S(49),S(23));
    disc(glass,x+S(80),y+S(49),S(16));
    disc(dark,x+S(80),y+S(49),S(9));
    disc(UI_TEXT,x+S(70),y+S(39),S(4));
    disc(dark,x+S(26),y+S(80),S(16));
    disc(stripe[0],x+S(26),y+S(80),S(12));
    disc(ICON_RGB(255,138,112),x+S(22),y+S(76),S(4));
    bmp_fill(COLOR_GRAY(30),x+S(33),y+S(103),S(94),S(4));
    bmp_fill(COLOR_GRAY(30),x+S(33),y+S(114),S(94),S(4));
#undef S
}

static void print_paper(unsigned progress)
{
    int height=(int)progress*60/100;
    if (height) bmp_fill(UI_TEXT,318,211,84,height);
    if (height>7) bmp_fill(UI_EDGE,323,214,74,height-7);
}

/* The ML UI occupies palette entries 0..79. A separate RGB cube in the
 * two 600D bitmap banks lets the unchanged RGB thumbnail use colour. */
#define CUBE_BASE 80
#define CUBE_SIZE (6 * 7 * 4)
static void photo_palette(int enable)
{
    if (!photo_palette_on && !enable) return;
    if (enable && photo_palette_on &&
        shamem_read(LCD_Palette[3 * (CUBE_BASE + 3)]) == palette_probe) return;
    uint32_t white = LCD_Palette[3 * COLOR_WHITE + 2];
    unsigned opacity = white >> 24, white_y = (white >> 16) & 255;
    if (opacity < 2) opacity = 3;
    if (white_y < 180) white_y = 235;
    for (int i = 0; i < CUBE_SIZE + 8; i++) {
        int slot = 3 * (CUBE_BASE + i);
        uint32_t entry = LCD_Palette[slot + 2];
        if (enable) {
            int r = (i / 28) * 255 / 5, g = ((i / 4) % 7) * 255 / 6, b = (i % 4) * 85;
            static const unsigned char theme[][3] = {
                {11,16,32},{21,27,51},{42,51,88},{244,246,255},
                {138,147,184},{25,195,255},{255,90,54},{61,220,151}
            };
            if (i >= CUBE_SIZE) {
                const unsigned char *rgb = theme[i-CUBE_SIZE];
                r=rgb[0]; g=rgb[1]; b=rgb[2];
            }
            int y = (299*r + 587*g + 114*b) / 1000;
            int u = (b - y) * 564 * (int)white_y / 255000;
            int v = (r - y) * 713 * (int)white_y / 255000;
            entry = (opacity << 24) | ((y * white_y / 255) << 16) |
                    ((uint32_t)(uint8_t)u << 8) | (uint8_t)v;
            if (i == 3) palette_probe = entry;
        }
        EngDrvOut(LCD_Palette[slot], entry);
        EngDrvOut(LCD_Palette[slot + 0x300], enable ? entry : LCD_Palette[slot + 0x302]);
    }
    photo_palette_on = enable;
}

static uint8_t photo_colour(const unsigned char *rgb, unsigned x, unsigned y)
{
    int r = rgb[0], g = rgb[1], b = rgb[2];
    int darkest = MIN(r, MIN(g,b)), lightest = MAX(r, MAX(g,b));
    if (lightest - darkest <= 6)
        return COLOR_GRAY((77*r + 150*g + 29*b) * 100 / (255*256));
    static const uint8_t bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    unsigned threshold = 8 + 16 * bayer[(y % 4)*4 + x % 4];
    return CUBE_BASE + 28 * ((r*5+threshold)/255) +
                       4 * ((g*6+threshold)/255) + ((b*3+threshold)/255);
}

static void photo_details(const char *label)
{
    char counter[40];
    rounded(UI_CYAN, 417, 80, 270, 43);
    text(FONT(FONT_MED_LARGE, UI_BG, UI_CYAN), 429, 85, label);
    const char *name=print_name;
    if (compare_original) {
        name=source_path;
        for (const char *p=source_path; *p; ++p)
            if (*p=='/') name=p+1;
    }
    text(FONT_MED_LARGE, 418, 145, name);
    unsigned bytes=compare_original ? source_bytes : print_bytes;
    if (bytes) {
        if (bytes>=1000000)
            snprintf(counter,sizeof(counter),"%d.%02d MB",(int)(bytes/1000000),(int)(bytes%1000000)/10000);
        else snprintf(counter,sizeof(counter),"%d.%d kB",(int)(bytes/1000),(int)(bytes%1000)/100);
        if (page==RESULT && !compare_original) {
            size_t used=strlen(counter);
            snprintf(counter+used,sizeof(counter)-used,"   %d.%d s",
                     (int)(print_elapsed_ms/1000),(int)(print_elapsed_ms/100)%10);
        }
        text(FONT_MED,418,384,counter);
    }
    if (photo_total) {
        snprintf(counter, sizeof(counter), "Print %d / %d", photo_index, photo_total);
        text(FONT_MED_LARGE, 418, 193, counter);
    }
    bmp_fill(COLOR_ORANGE, 418, 247, 42, 4);
    text(FONT_MED, 418, 264, !has_capture_date && photo_date[0] ? "File time" : "Captured");
    if (photo_date[0]) {
        char day[11];
        memcpy(day, photo_date, 10);
        day[4] = day[7] = '-';
        day[10] = 0;
        text(FONT_MED_LARGE, 418, 300, day);
        text(FONT_MED_LARGE, 418, 340, photo_date + 11);
    } else {
        text(FONT_MED_LARGE, 418, 300, "Not recorded");
    }
}

/* Scale the existing thumbnail at display time so each shot keeps the
 * same camera RAM budget. The framed JPEG on the card stays 1080 square. */
#define PHOTO_SIDE 360
static void render_preview(int left, int top, unsigned side, unsigned progress)
{
    uint8_t *vram = bmp_vram();
    const unsigned char *pixels = compare_original ? original_preview : preview;
    if (!vram || !pixels) return;
    for (unsigned y = 0; y < progress * side / 100; ++y) {
        const unsigned char *row = pixels + 3 * CR_PREVIEW_SIZE * (y * CR_PREVIEW_SIZE / side);
        for (unsigned x = 0; x < side; ++x) {
            const unsigned char *rgb = row + 3 * (x * CR_PREVIEW_SIZE / side);
            bmp_putpixel_fast(vram, left + x, top + y, photo_colour(rgb, x, y));
        }
    }
}

static void progress_widgets(unsigned progress, unsigned first)
{
    const char *steps[] = {"Develop", frame==CR_NO_FRAME ? "Tone" : "Frame", "Write JPEG"};
    int stage = progress >= 85 ? 2 : progress >= 5 ? 1 : 0;
    if (!first || first <= 1 || progress >= 85) {
        for (int i=0;i<3;++i) {
            int ink=i<stage ? UI_MINT : i==stage ? UI_CYAN : UI_MUTED;
            text(FONT(FONT_MED_LARGE,ink,UI_BG),PROGRESS_X+218*i,329,steps[i]);
        }
    }
    unsigned filled = progress / 5;
    for (unsigned i=first; i<(first ? filled : PROGRESS_COUNT); ++i)
        bmp_fill(i<filled ? UI_CYAN : UI_EDGE,PROGRESS_X+i*(BLOCK_W+BLOCK_GAP),
                 PROGRESS_Y,BLOCK_W,BLOCK_H);
    char label[12];
    snprintf(label,sizeof(label),"%d%s",(int)progress,"%");
    text(FONT(FONT_MED_LARGE,UI_CYAN,UI_PANEL),604,12,label);
}

static const int chips[] = {COLOR_YELLOW, COLOR_CYAN, UI_BRAND, UI_TEXT, UI_MUTED};

static void tone_sample(void)
{
    static const uint8_t rgb[4][5][3] = {
        {{51,50,51},{105,97,91},{157,143,123},{199,184,152},{228,216,192}},
        {{26,43,103},{219,54,69},{255,175,32},{40,184,114},{32,186,234}},
        {{56,33,47},{102,48,44},{164,69,36},{228,114,40},{252,191,87}},
        {{22,22,22},{65,65,65},{111,111,111},{173,173,173},{230,230,230}}
    };
    bmp_fill(UI_PANEL,444,258,252,104);
    if (selection==4) {
        text(FONT(FONT_MED_LARGE,UI_TEXT,UI_PANEL),471,297,"Saved prints");
        return;
    }
    for (int i=0;i<5;++i)
        bmp_fill(photo_colour(rgb[selection][i],0,0),458+45*i,270,42,31);
    text(FONT(FONT_MED_LARGE,UI_TEXT,UI_PANEL),461,314,home_labels[selection]);
}

static void home_card(int i)
{
    int y = CARD_Y + i*(CARD_H+CARD_GAP) + (i==4 ? 8 : 0);
    int fill = selection==i ? UI_EDGE : UI_PANEL;
    bmp_fill(fill, CARD_X+5, y, CARD_W-10, CARD_H);
    bmp_fill(fill, CARD_X, y+5, CARD_W, CARD_H-10);
    bmp_draw_rect_chamfer(selection==i ? UI_CYAN : fill, CARD_X,y,CARD_W,CARD_H,5,1);
    bmp_fill(chips[i], CARD_X+16,y+14,24,24);
    text(FONT(FONT_MED_LARGE,UI_TEXT,fill),CARD_X+56,y+11,home_labels[i]);
}

static void draw(unsigned progress)
{
    if (!display_available())
        return;
    BMP_LOCK(
        bmp_draw_to_idle(1);
        switch (page) {
        case INFO_COVER:
        case COVER:
            bmp_fill(UI_BG,0,0,720,480);
            camera_icon(280,112,160);
            text(FONT(FONT_LARGE,UI_TEXT,UI_BG) | FONT_ALIGN_CENTER,360,246,
                 "Canoroid v" CANOROID_VERSION);
            text(FONT(FONT_MED_LARGE,UI_TEXT,UI_BG) | FONT_ALIGN_CENTER,360,297,
                 "Turn your EOS into an instant cam");
            text(FONT(FONT_MED,UI_CYAN,UI_BG) | FONT_ALIGN_CENTER,360,345,
                 "github.com/pongallery/Canoroid");
            break;
        case HOME:
            heading("CANOROID", NULL);
            text(FONT(FONT_MED, UI_TEXT, UI_PANEL), 233, 18, "Turn your EOS into an instant cam");
            text(FONT_MED_LARGE, CARD_X, 65, "Choose a tone");
            camera_icon(490, 121, 160);
            tone_sample();
            for (int i=0;i<5;++i) home_card(i);
            key_action(24,KEY_VERTICAL,"Choose");
            key_action(252,KEY_SET,"Open");
            key_action(480,KEY_MENU,"Canon");
            break;
        case FRAMES:
            heading("Choose a frame", NULL);
            arrow(29,199,14,1,-1,UI_CYAN);
            arrow(689,199,14,1,1,UI_CYAN);
            key_action(24,KEY_HORIZONTAL,"Browse");
            key_action(252,KEY_SET,"Live View");
            key_action(480,KEY_PLAY,"Home");
            for (int slot = 0; slot < 3; ++slot) {
                int i = frame / 3 * 3 + slot;
                if (i >= CR_FRAME_COUNT) break;
                int x = 58 + slot * 213;
                if (i == frame)
                    bmp_draw_rect(COLOR_CYAN, x - 7, 115, 166, 166);
                if (i==CR_NO_FRAME) {
                    bmp_fill(UI_EDGE,x,151,152,100);
                    text(FONT(FONT_MED,UI_TEXT,UI_EDGE) | FONT_ALIGN_CENTER,x+76,188,"Full size");
                    text(FONT_MED,x-4,295,cr_frames[i].name);
                    continue;
                }
                uint8_t *bitmap = bmp_vram();
                for (int py = 0; bitmap && py < 152; ++py)
                    for (int px = 0; px < 152; ++px) {
                        const uint8_t *rgb = cr_paper(&cr_frames[i], px*1080/152, py*1080/152);
                        bmp_putpixel_fast(bitmap, x+px, 122+py, photo_colour(rgb, px, py));
                    }
                bmp_fill(COLOR_GRAY(31), x + cr_frames[i].inset * 152 / 1080, 122 + cr_frames[i].top * 152 / 1080, (1080 - 2 * cr_frames[i].inset) * 152 / 1080, cr_frames[i].height * 152 / 1080);
                text(FONT_MED, x - 4, 295, cr_frames[i].name);
            }
            text(FONT_MED, 58, 363, frame_hint ? frame_hint : home_labels[selection]);
            break;
        case DEVELOPING:
            heading("Making your print", "Original JPEG and RAW kept");
            camera_icon(290, 111, 140);
            print_paper(progress);
            text(FONT_MED_LARGE, 260, 282, waiting_jpeg ? "Waiting for JPEG" :
                 progress >= 90 ? "Checking JPEG" : "Making print");
            progress_widgets(progress, 0);
            break;
        case RESULT:
            heading("Your print", NULL);
            photo_actions(0);
            bmp_fill(UI_TEXT, 32, 57, 372, 369);
            render_preview(38, 62, PHOTO_SIDE, progress);
            photo_details(compare_original ? "Original" : home_labels[mono ? 3 : tone - 1]);
            break;
        case GALLERY:
            heading("Saved photos", NULL);
            photo_actions(1);
            if (gallery_loaded) {
                render_preview(38, 62, PHOTO_SIDE, 100);
                photo_details("Saved JPEG");
            } else {
                text(FONT_MED, 48, 125, print_name);
                text(FONT_SMALL, 48, 190, error);
            }
            break;
        case DELETE_PRINT:
            heading("Delete this print?", NULL);
            render_preview(48, 93, 172, 100);
            text(FONT_MED_LARGE, 246, 111, print_name);
            text(FONT_MED, 246, 170, "Delete saved copy only");
            text(FONT_MED, 246, 207, "Original JPEG / RAW kept");
            button_face(46, 303, 306, 68, delete_selected ? UI_PANEL : UI_CYAN,
                        delete_selected ? UI_TEXT : UI_BG, "Cancel");
            button_face(367, 303, 306, 68, delete_selected ? UI_BRAND : UI_PANEL,
                        delete_selected ? UI_BG : UI_TEXT, "Delete print");
            key_action(24,KEY_HORIZONTAL,"Select");
            key_action(252,KEY_SET,"Confirm");
            key_action(480,KEY_PLAY,"Back");
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
            photo_palette(1);
            bmp_idle_copy(1, 0);
            last_presented = bmp_vram_real();
            last_display_check = get_ms_clock();
            if (page == COVER && !cover_presented) {
                cover_since = last_display_check;
                cover_presented = 1;
            }
        }
    )
}

static void advance_print(unsigned progress)
{
    if (progress <= 100 && !leave_requested) {
        claim_review_screen();
        static unsigned last;
        uint8_t *front=bmp_vram_real();
        if (progress >= 10 && progress < 90 && last >= 5 && last < progress &&
            display_available() && front && front == last_presented && screen_signature(front)==last_signature) {
            BMP_LOCK(
                progress_widgets(progress,last/5);
                print_paper(progress);
                last_signature=screen_signature(front);
            )
        } else draw(progress);
        last=progress;
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
    compare_original = gallery_loaded = reveal_pending = 0;
}

/* DryOS directory entries on DIGIC 4 use seconds since 1970 (as in
 * ML file_man). Label this fallback separately from EXIF capture time. */
static void format_file_time(unsigned stamp)
{
    static const int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!stamp) return;
    unsigned days = stamp / 86400;
    int year = 1970, month = 1;
    for (;;) {
        int leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        unsigned length = days_in_month[month-1] + (month == 2 && leap);
        if (days < length) break;
        days -= length;
        if (++month == 13) { year++; month = 1; }
    }
    snprintf(photo_date, sizeof(photo_date), "%04d:%02d:%02d %02d:%02d:%02d",
             year, month, (int)days+1, (int)(stamp/3600%24), (int)(stamp/60%60), (int)(stamp%60));
}

/* Count by the same DCF/legacy ordering used for gallery navigation. */
static void load_details(const char *path)
{
    if (FIO_GetFileSize(path,&print_bytes)) print_bytes=0;
    cr_capture_date(path, photo_date);
    has_capture_date = photo_date[0] != 0;
    photo_total = photo_index = 0;
    struct fio_file entry;
    for (unsigned location = 0; location < 2; ++location) {
        struct fio_dirent *dir = FIO_FindFirstEx(print_dirs[location], &entry);
        if (!dir || IS_ERROR(dir)) continue;
        do {
            if (entry.mode & ATTR_DIRECTORY) continue;
            unsigned k = photo_key(entry.name);
            if (!k) continue;
            k = k * 2 + location;
            photo_total++;
            if (k <= gallery_position) photo_index++;
            if (k == gallery_position && !has_capture_date) format_file_time(entry.timestamp);
        } while (FIO_FindNextEx(dir, &entry) == 0);
        FIO_FindClose(dir);
    }
}

static void browse_prints(int direction)
{
    release_preview();
    print_name[0] = 0;
    page = GALLERY;
    struct fio_file entry;
    unsigned adjacent = 0, wrap = 0;
    char wrap_name[13] = "";
    for (unsigned location = 0; location < 2; ++location) {
        struct fio_dirent *dir = FIO_FindFirstEx(print_dirs[location], &entry);
        if (!dir || IS_ERROR(dir)) continue;
        do {
            if (entry.mode & ATTR_DIRECTORY) continue;
            unsigned k = photo_key(entry.name);
            if (!k) continue;
            k = k * 2 + location;
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
    }
    if (!wrap) {
        gallery_position = 0;
        snprintf(error, sizeof(error), "No saved prints on this card");
        return;
    }
    if (!adjacent) snprintf(print_name, sizeof(print_name), "%s", wrap_name);
    gallery_position = adjacent ? adjacent : wrap;
    char path[48];
    snprintf(path, sizeof(path), "%s/%s", print_dirs[gallery_position % 2], print_name);
    preview = malloc(PREVIEW_BYTES);
    card_read_buffer = fio_malloc(CR_IO_CHUNK);
    if (preview && card_read_buffer)
        gallery_loaded = cr_load_print(path, preview, error);
    else
        snprintf(error, sizeof(error), "Gallery memory allocation failed");
    if (gallery_loaded) load_details(path);
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
    snprintf(report, sizeof(report), "Canoroid " CANOROID_VERSION "\nInput: %s\nFrame: %s\n%s\nDCIM %d / file %d\n\n",
             source_path[0] ? source_path : "(unresolved)", cr_frames[frame].name,
             error, card->folder_number, card->file_number);
    for (int n = 1; n <= 9999; ++n) {
        snprintf(path, sizeof(path), LEGACY_DIR "/E%04d.TXT", n);
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
    advance_print(0);
    memset(pixels + bytes, 0, bytes);
    struct cr_frame style = cr_frames[frame];
    style.monochrome = mono;
    style.tone = mono ? 0 : tone;
    unsigned started=get_ms_clock();
    int printed = cr_print(source_path, temporary, destination, &style,
                           pixels, pixels + bytes, advance_print, NULL, error);
    if (printed) {
        print_elapsed_ms=(unsigned)get_ms_clock()-started;
        gallery_position = photo_key(print_name) * 2 + 1;
        load_details(destination);
        if (FIO_GetFileSize(source_path,&source_bytes)) source_bytes=0;
    }
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
    page = RESULT;
    reveal_pending = 1;
}

static void show_finished_page(void)
{
    claim_review_screen();
    if (!display_available()) return;
    if (page == RESULT && reveal_pending) {
        reveal_pending = 0;
        for (unsigned progress = 12; progress < 100 && !leave_requested; progress += 12) {
            claim_review_screen();
            if (!display_available()) return;
            draw(progress);
            msleep(35);
        }
    }
    draw(100);
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
        if (button == BGMT_TRASH && can_open()) {
            page = COVER;
            open_requested = 1;
            return 0;
        }
        return 1;
    }

    if (page == INFO_COVER) {
        switch (button) {
        case BGMT_INFO:
        case BGMT_UNPRESS_SET: case BGMT_UNPRESS_LEFT: case BGMT_UNPRESS_RIGHT:
        case BGMT_UNPRESS_UP: case BGMT_UNPRESS_DOWN: case BGMT_UNPRESS_DISP:
        case BGMT_UNPRESS_ZOOM_IN: case BGMT_UNPRESS_ZOOM_OUT:
            return 0;
        case BGMT_MENU: case BGMT_PLAY: case BGMT_TRASH: case BGMT_LV:
        case BGMT_PRESS_SET: case BGMT_PRESS_UP: case BGMT_PRESS_DOWN:
        case BGMT_PRESS_LEFT: case BGMT_PRESS_RIGHT: case BGMT_PRESS_DISP:
        case BGMT_PRESS_HALFSHUTTER: case BGMT_Q: case BGMT_Q_ALT_: case BGMT_ISO:
        case BGMT_PRESS_ZOOM_IN: case BGMT_PRESS_ZOOM_OUT:
        case BGMT_WHEEL_UP: case BGMT_WHEEL_DOWN: case BGMT_WHEEL_LEFT: case BGMT_WHEEL_RIGHT:
            post_key(button);
            return 0;
        default:
            return 1;
        }
    }
    if (active && page==HOME && button==BGMT_INFO) {
        post_key(button);
        return 0;
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
        if (opening || open_requested || page == COVER) return 0;
        if (page == GALLERY || page == DELETE_PRINT) post_key(button);
        else if (page != DEVELOPING) leave_requested = 1;
        return 0;
    case BGMT_PRESS_UP:
    case BGMT_PRESS_DOWN:
    case BGMT_PRESS_LEFT:
    case BGMT_PRESS_RIGHT:
    case BGMT_PRESS_SET:
    case BGMT_PLAY:
        if (page != DEVELOPING && page != COVER)
            post_key(button);
        return 0;
    case BGMT_ISO:
    case BGMT_Q:
    case BGMT_Q_ALT_:
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

static uint8_t *rail_buffer;
/* The lower Canon exposure display owns the bottom of the LCD.
 * Only cover the surrounding live image above that area. */
/* Five clear samples per 4x4 tile. Anchor it to LCD coordinates
 * so refreshing the bitmap never shifts the pattern. */
static uint8_t mask_pixel(int x,int y)
{
    static const uint8_t open_bits[4]={5,0,5,2};
    return open_bits[y&3] & (1<<(x&3)) ? COLOR_EMPTY : COLOR_ALMOST_BLACK;
}

static void mask_area(uint8_t *front,int x,int y,int w,int h)
{
    if (w<=0 || h<=0) return;
    if (front==rail_buffer && front[y*BMPPITCH+x]==mask_pixel(x,y) &&
        front[(y+h/2)*BMPPITCH+x+w/2]==mask_pixel(x+w/2,y+h/2) &&
        front[(y+h-1)*BMPPITCH+x+w-1]==mask_pixel(x+w-1,y+h-1) &&
        front[(y+MIN(h-1,1))*BMPPITCH+x]==mask_pixel(x,y+MIN(h-1,1))) return;
    uint8_t row[720];
    for (int phase=0; phase<4 && phase<h; ++phase) {
        for (int i=0; i<w; ++i) row[i]=mask_pixel(x+i,y+phase);
        for (int j=phase; j<h; j+=4) memcpy(front+(y+j)*BMPPITCH+x,row,w);
    }
}

static void guide(void)
{
    if (frame==CR_NO_FRAME) return;
    get_yuv422_vram();
    int side=MIN(os.x_ex,os.y_ex);
    int x=os.x0+(os.x_ex-side)/2, y=os.y0+(os.y_ex-side)/2;
    if (side<100 || x<0 || y<0 || x+side>720 || y+side>480) return;
    const struct cr_frame *f=&cr_frames[frame];
    int left=x+side*f->inset/1080, right=x+side*(1080-f->inset)/1080;
    int top=y+side*f->top/1080, bottom=y+side*(f->top+f->height)/1080;
    BMP_LOCK(
        uint8_t *front=bmp_vram_real();
        if (front) {
            int limit=420;
            mask_area(front,0,0,720,MIN(top,limit));
            mask_area(front,0,top,left,MIN(bottom,limit)-top);
            mask_area(front,right,top,720-right,MIN(bottom,limit)-top);
            mask_area(front,0,bottom,720,limit-bottom);
            rail_buffer=front;
        }
        bmp_fill(COLOR_CYAN,left,top,right-left,2);
        bmp_fill(COLOR_CYAN,left,bottom-2,right-left,2);
        bmp_fill(COLOR_CYAN,left,top,2,bottom-top);
        bmp_fill(COLOR_CYAN,right-2,top,2,bottom-top);
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
    awaiting_display = 0;
    active = 0;
    cancel_shot = 0;
    shooting = 1;
    BMP_LOCK(bmp_fill(COLOR_EMPTY, 0, 0, 720, 480); photo_palette(0);)
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
    rail_buffer=NULL;
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
        else if (rail_buffer) {
            rail_buffer=NULL;
            if (DISPLAY_IS_ON && !QR_MODE) redraw();
        }
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
    awaiting_display = 1;
    last_presented = candidate_buffer = NULL;
    waiting_jpeg = 1;
    uint32_t last = 0;
    int ready = 0;
    unsigned stable = 0;
    const char *pending = "Camera JPEG has not appeared";
    for (int i = 0; saw_capture && i < 250 && !ml_shutdown_requested; i++) {
        claim_review_screen();
        /* Waiting has no percentage yet. Refresh only if Canon replaced it. */
        uint8_t *front = bmp_vram_real();
        if (display_available() && front && (last_presented != front ||
            screen_signature(front) != last_signature)) draw(0);
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
    show_finished_page();
}

static void handle_key(int button)
{
    if (page==INFO_COVER) {
        page=HOME;
        key_read=key_write;
        return;
    }
    if (page==HOME && button==BGMT_INFO) {
        page=INFO_COVER;
        return;
    }
    if (page == DELETE_PRINT) {
        if (button == BGMT_PRESS_LEFT) delete_selected = 0;
        if (button == BGMT_PRESS_RIGHT) delete_selected = 1;
        if (button == BGMT_PLAY || button == BGMT_TRASH) page = GALLERY;
        if (button == BGMT_PRESS_SET) {
            page = GALLERY;
            if (display_available() && delete_selected && gallery_loaded && gallery_position &&
                photo_key(print_name) == gallery_position / 2) {
                char path[48];
                snprintf(path, sizeof(path), "%s/%s", print_dirs[gallery_position % 2], print_name);
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
        int choices = 5;
        if (button == BGMT_PRESS_UP) selection = (selection + choices - 1) % choices;
        if (button == BGMT_PRESS_DOWN) selection = (selection + 1) % choices;
        if (button == BGMT_PRESS_SET && selection < 4) {
            mono = selection == 3;
            tone = mono ? 0 : selection + 1;
            page = FRAMES;
        }
        if (button == BGMT_PRESS_SET && selection == 4) {
            gallery_position = 0;
            browse_prints(-1);
        }
    } else if (page == FRAMES) {
        if (button == BGMT_PRESS_LEFT)
            frame = (frame + CR_FRAME_COUNT - 1) % CR_FRAME_COUNT;
        if (button == BGMT_PRESS_RIGHT)
            frame = (frame + 1) % CR_FRAME_COUNT;
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

static int enter_still_menu(void)
{
    /* In Live View the 600D cross keys can be WB/AF/drive shortcuts.
     * Use Canon's normal still-photo menu for interactive Canoroid pages. */
    if (is_movie_mode() || ml_shutdown_requested) return 0;
    if (lv) {
        SetGUIRequestMode(0);
        for (int i = 0; i < 60 && CURRENT_GUI_MODE && !leave_requested &&
             !ml_shutdown_requested; i++) msleep(20);
        if (CURRENT_GUI_MODE || leave_requested || ml_shutdown_requested) return 0;
        if (lv) fake_simple_button(BGMT_LV);
        for (int i = 0; i < 100 && lv && !leave_requested && !ml_shutdown_requested; i++) msleep(20);
    }
    if (lv || leave_requested || ml_shutdown_requested) return 0;
    gui_mode = GUIMODE_MENU;
    SetGUIRequestMode(gui_mode);
    for (int i = 0; i < 100 && CURRENT_GUI_MODE != gui_mode &&
         !ml_shutdown_requested && !leave_requested; ++i) msleep(10);
    msleep(50);
    return !lv && CURRENT_GUI_MODE == gui_mode && DISPLAY_IS_ON &&
           !get_halfshutter_pressed() && !ml_shutdown_requested && !leave_requested;
}

static void canoroid_task(void *unused)
{
    (void) unused;
    extern int ml_started;
    extern int ml_gui_initialized;
    while (!ml_started || !ml_gui_initialized)
        msleep(100);
    FILE *log = FIO_CreateFile(LEGACY_DIR "/STARTUP.TXT");
    if (log) {
        const char message[] = "Canoroid " CANOROID_VERSION ": task and Canon GUI ready.\n";
        FIO_WriteFile(log, message, sizeof(message) - 1);
        FIO_CloseFile(log);
    }
    unsigned retries = 0;
    for (;;) {
        if (open_requested && (active || can_open()) && !leave_requested) {
            open_requested = 0;
            opening = 1;
            awaiting_display = 0;
            active = 0;
            retained_page = 0;
            if (photo_palette_on) { BMP_LOCK(photo_palette(0);) }
            if (canon_gui_front_buffer_disabled()) canon_gui_enable_front_buffer(0);
            if (enter_still_menu()) {
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
            if (!retained_page && (lv || CURRENT_GUI_MODE != gui_mode)) {
                /* A closed dialog leaves the last bitmap visible but can
                 * stop producing menu key events. Reopen the dialog itself. */
                if (can_open()) open_requested = 1;
                else leave_requested = 1;
            } else if (page == COVER && cover_presented && display_available() &&
                       (unsigned)get_ms_clock() - cover_since >= 2000) {
                page = HOME;
                cover_presented = 0;
                draw(100);
            } else if (awaiting_display) {
                show_finished_page();
            } else if (usable_screen() && (unsigned)get_ms_clock() - last_display_check >= 250) {
                last_display_check = get_ms_clock();
                uint8_t *front = bmp_vram_real();
                if (front && (!display_available() || last_presented != front ||
                              screen_signature(front) != last_signature ||
                              (photo_palette_on && shamem_read(LCD_Palette[3*(CUBE_BASE+3)]) != palette_probe))) {
                    claim_review_screen();
                    draw(100);
                }
            }
        }
        if (leave_requested) {
            int to_menu = leave_requested == 2;
            awaiting_display = 0;
            cover_presented = 0;
            active = 0;
            open_requested = 0;
            key_read = key_write;
            if (!ml_shutdown_requested) {
                if (canon_gui_front_buffer_disabled()) {
                    BMP_LOCK(bmp_fill(COLOR_EMPTY, 0, 0, 720, 480); photo_palette(0);)
                    canon_gui_enable_front_buffer(0);
                }
                if (photo_palette_on) { BMP_LOCK(photo_palette(0);) }
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
        if (active && !awaiting_display && !open_requested && DISPLAY_IS_ON && !ml_shutdown_requested &&
            key_read != key_write && (CURRENT_GUI_MODE == gui_mode ||
            (retained_page && key_queue[key_read] == BGMT_PLAY))) {
            int button = key_queue[key_read];
            key_read = (key_read + 1) % KEY_SLOTS;
            int previous = selection;
            int was_home = page==HOME;
            handle_key(button);
            if (was_home && page==HOME && previous!=selection && display_available() &&
                last_presented && last_presented==bmp_vram_real() && screen_signature(last_presented)==last_signature) {
                BMP_LOCK(home_card(previous); home_card(selection); tone_sample(); last_signature=screen_signature(last_presented);)
            } else if (active && !open_requested && !leave_requested) draw(100);
        }
        msleep(40);
    }
}

TASK_CREATE("canoroid", canoroid_task, 0, 0x1e, 0x8000);
