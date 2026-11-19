/* SPDX-License-Identifier: GPL-2.0-only
 * Host regression for card contents, GUI transitions and the camera JPEG path.
 * tests/card_session.py builds this with empty platform headers and the same
 * IJG objects as canoroid-print. Only Canon hardware and DryOS calls are mocked.
 */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { BGMT_ISO=70, BGMT_Q, BGMT_Q_ALT_, BGMT_MENU=1, BGMT_PLAY, BGMT_TRASH, BGMT_PRESS_HALFSHUTTER, BGMT_LV,
    BGMT_PRESS_UP, BGMT_PRESS_DOWN, BGMT_PRESS_LEFT, BGMT_PRESS_RIGHT, BGMT_PRESS_SET,
    BGMT_UNPRESS_UP, BGMT_UNPRESS_DOWN, BGMT_UNPRESS_LEFT, BGMT_UNPRESS_RIGHT, BGMT_UNPRESS_SET };
enum { BGMT_INFO=100,BGMT_PRESS_DISP,BGMT_UNPRESS_DISP,BGMT_PRESS_ZOOM_IN,BGMT_UNPRESS_ZOOM_IN,BGMT_PRESS_ZOOM_OUT,BGMT_UNPRESS_ZOOM_OUT,BGMT_WHEEL_UP,BGMT_WHEEL_DOWN,BGMT_WHEEL_LEFT,BGMT_WHEEL_RIGHT };
enum { SHOOTMODE_P, SHOOTMODE_AV, SHOOTMODE_TV, SHOOTMODE_M };
static int shooting_mode=SHOOTMODE_P, lv, lv_dispsize=1, qr, current_mode, disabled;
static int busy, unavailable_dcim, refuse_report, ml_shutdown_requested;
int ml_started=1, ml_gui_initialized=1;
#define QR_MODE qr
#define MENU_MODE (current_mode==2)
#define PLAY_MODE 0
#define CURRENT_GUI_MODE current_mode
#define GUIMODE_ML_MENU (lv ? 68 : 2)
#define GUIMODE_MENU 2
enum { GUIMODE_WB=5, GUIMODE_DRIVE_MODE=8, GUIMODE_FOCUS_MODE=9, GUIMODE_Q_UNAVI=31, GUISTATE_QMENU=9 };
static int gui_state, image_review_time=2;
#define GUISTATE_IDLE 0
#define get_ml_topbar_pos() 2
#define get_ml_bottombar_pos() 446
#define FONT_ALIGN_CENTER 0
#define DISPLAY_IS_ON 1
#define get_halfshutter_pressed() 0
#define is_movie_mode() 0
#define liveview_display_idle() (!qr)
#define job_state_ready_to_take_pic() (!busy)
#define canon_gui_front_buffer_disabled() disabled
#define canon_gui_disable_front_buffer() (disabled=1)
#define canon_gui_enable_front_buffer(x) (disabled=0)
#define SetGUIRequestMode(mode) (current_mode=(mode))
#define fake_simple_button(k) (lv=!lv)
#define BMP_LOCK(x) {x}
#define COLOR_RED 8
#define COLOR_GREEN1 6
static struct {int iso, iso_auto;} lens_info = {400,0};
static int battery_level_bars=2;
static int testing_guide;
static unsigned char guide_pixels[480][720];
static void record_fill(int color, int x, int y, int w, int h) {
    assert(x>=0 && y>=0 && w>=0 && h>=0 && x+w<=720 && y+h<=480);
    if (testing_guide)
        for (int row=y; row<y+h; ++row)
            memset(guide_pixels[row]+x,color,w);
}
#define bmp_fill record_fill
#define bmp_draw_rect(...) ((void)0)
#define bmp_draw_rect_chamfer(...) ((void)0)
static char displayed_error[100];
static int developing_draws;
static void record_label(int font, int x, int y, const char *fmt, ...) {
    char label[128];
    va_list args;
    va_start(args,fmt);
    vsnprintf(label,sizeof(label),fmt,args);
    va_end(args);
    if (!strcmp(label, "Making your print")) developing_draws++;
    if (x == 48 && y >= 171 && y <= 264) {
        if (y == 171) displayed_error[0] = 0;
        assert(strlen(displayed_error) + strlen(label) < sizeof(displayed_error));
        strcat(displayed_error, label);
    }
}
#define bmp_printf record_label
static uint8_t back[720 * 480], front[720 * 480];
static unsigned sim_ms, presents, preview_pixels, qr_presents;
static int drawing_back;
#define COLOR_BLACK 3
#define COLOR_EMPTY 0
#define COLOR_ALMOST_BLACK 38
#define COLOR_ORANGE 42
#define COLOR_WHITE 1
static uint32_t LCD_Palette[0x600], registers[512];
static uint32_t shamem_read(unsigned reg) { assert(reg<512); return registers[reg]; }
static void EngDrvOut(unsigned reg, uint32_t entry) { assert(reg<512); registers[reg]=entry; }
#define MAX(a,b) ((a)>(b)?(a):(b))
#define bmp_draw_to_idle(i) (drawing_back=(i))
#define bmp_vram() (drawing_back ? back : front)
#define BMPPITCH 720
#define bmp_vram_idle() back
static uint8_t *bmp_vram_real(void) {return front;}
#define bmp_getpixel(x,y) front[(y)*720+(x)]
#define get_ms_clock() sim_ms
static void bmp_idle_copy(int direction, int full) {
    assert(!drawing_back && direction==1 && !full);
    memcpy(front,back,sizeof(front));

    presents++;
    if (qr) qr_presents++;
}
static void bmp_putpixel_fast(uint8_t *vram, int x, int y, uint8_t color) {
    assert(drawing_back && vram==back && x>=0 && x<720 && y>=0 && y<480);
    vram[y*720+x]=color;
    preview_pixels++;
}
#define bmp_on() ((void)0)
#define redraw() ((void)0)
#define get_yuv422_vram() ((void)0)
#define TASK_CREATE(...)
enum {FONT_SMALL, FONT_MED, FONT_LARGE, FONT_MED_LARGE, COLOR_CYAN, COLOR_ALMOST_WHITE, COLOR_YELLOW};
#define COLOR_GRAY(n) (38+(n)*41/100)
#define FONT(...) 0
#define MIN(a,b) ((a)<(b)?(a):(b))
static struct {int x0,y0,x_ex,y_ex;} os={0,0,720,480};

#define ATTR_DIRECTORY 16
#define IS_ERROR(p) ((p)==(void *)-1)
struct card_info {char *drive_letter; int file_number, folder_number;};
static struct card_info card={"B",100,100};
static struct card_info *get_ml_card(void) { return &card; }
static struct card_info *get_shooting_card(void) { return &card; }
#define get_dcim_dir_suffix() "CANON"
#define get_file_prefix() "IMG_"
#define fio_malloc malloc
#define fio_free free
#define FIO_OpenFile(p,flags) fopen(p,"rb")
#define FIO_CreateFileOrAppend(p) fopen(p,"ab")
static int tmp_denials;
static FILE *FIO_CreateFile(const char *p) {
    if (tmp_denials && strstr(p, ".TMP")) {tmp_denials--; return NULL;}
    if (refuse_report && strstr(p, "/E") && strstr(p, ".TXT")) return NULL;
    return fopen(p,"wb");
}
#define FIO_CreateDirectory(p) mkdir(p,0777)
#define FIO_CloseFile fclose
#define FIO_ReadFile(f,p,n) ((int)fread(p,1,n,f))
#define FIO_WriteFile(f,p,n) ((int)fwrite(p,1,n,f))
#define FIO_SeekSkipFile fseek
#define FIO_RenameFile rename
static int deny_delete;
static int FIO_RemoveFile(const char *path) { return deny_delete ? -1 : unlink(path); }
static int is_dir(const char *p) {struct stat s; return !stat(p,&s) && S_ISDIR(s.st_mode);}
static int is_file(const char *p) {struct stat s; return !stat(p,&s) && S_ISREG(s.st_mode);}
static int FIO_GetFileSize(const char *path,uint32_t *size) {
    struct stat s;
    if (stat(path,&s)) return -1;
    *size=s.st_size;
    return 0;
}
struct fio_file {int mode; unsigned timestamp; char name[256];};
struct fio_dirent {DIR *dir;};
static int FIO_FindNextEx(struct fio_dirent *it,struct fio_file *file) {
    struct dirent *e=readdir(it->dir);
    if (!e) return -1;
    struct stat st;
    assert(!fstatat(dirfd(it->dir),e->d_name,&st,0));
    file->timestamp=st.st_mtime;
    file->mode=S_ISDIR(st.st_mode) ? ATTR_DIRECTORY : 0;
    strcpy(file->name,e->d_name);
    return 0;
}
static void FIO_FindClose(struct fio_dirent *it) {closedir(it->dir); free(it);}
static struct fio_dirent *FIO_FindFirstEx(const char *p,struct fio_file *file) {
    if (unavailable_dcim && strstr(p,"/DCIM")) return (void *)-1;
    DIR *d=opendir(p);
    if (!d) return NULL;
    struct fio_dirent *it=malloc(sizeof(*it));
    it->dir=d;
    if (!FIO_FindNextEx(it,file)) return it;
    FIO_FindClose(it);
    return NULL;
}
static void msleep(int ms);
/* Host printf would mask both failure modes seen on the camera. Keep UI
 * and path generation within the small set used successfully by DryOS. */
static int camera_format(char *buf, size_t size, const char *fmt, ...)
{
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') continue;
        ++p;
        while (*p >= '0' && *p <= '9') ++p;
        assert(*p && strchr("sd%", *p));
    }
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return result;
}
#define snprintf camera_format
#include "../../../src/canoroid/camera.c"
#undef snprintf

static int step, restarted, shutter_ticks, total_ticks;
static jmp_buf finished;
static void press(int button) {assert(canoroid_handle_key(button)==0);}
/* Count chromatic palette entries only in the photo's interior (the
 * monochrome print may have a deliberately coloured paper border). */
static int colour_pixels(void)
{
    int count=0;
    for (int y=145;y<330;y++)
        for (int x=122;x<307;x++) {
            unsigned index=front[y*720+x];
            if (index>=CUBE_BASE && index<CUBE_BASE+CUBE_SIZE &&
                (registers[index] & 0xffff)) count++;
        }
    return count;
}

static void msleep(int ms)
{
    sim_ms += ms;
    if (++total_ticks == 2500) { fprintf(stderr, "Timeout step %d page %d active %d gui %d/%d shot %d now %u hash %u/%u frame %u\n",step,page,active,current_mode,gui_mode,shooting,sim_ms,screen_signature(front),last_signature,presents); abort(); }
    static unsigned idle_ticks, cover_start;
    if (step==0 && ms==40 && !active && !opening && !open_requested) {
        assert(!disabled && !photo_palette_on);
        assert(canoroid_handle_key(BGMT_PRESS_SET)==1);
        if (++idle_ticks==100) press(BGMT_TRASH);
        return;
    }
    if (active && page==COVER && ms==40) {
        assert(cover_presented && display_available());
        if (!cover_start) {
            cover_start=cover_since;
            press(BGMT_PRESS_SET);
        }
        assert(sim_ms-cover_start<=2040);
        return;
    }
    if (active && page==HOME && cover_start && ms==40) {
        assert(sim_ms-cover_start>=2000 && sim_ms-cover_start<=2080 && selection==0);
        cover_start=0;
    }
    if (!active && !opening && !open_requested && !shooting && !restarted && step==9 && ms==40) {
        step++;
        assert(!disabled && current_mode==GUIMODE_MENU);
        assert(!photo_palette_on && registers[CUBE_BASE+3]==LCD_Palette[3*(CUBE_BASE+3)+2]);
        current_mode=GUIMODE_Q_UNAVI;
        gui_state=GUISTATE_QMENU;
        lv=1;
        press(BGMT_TRASH);
        press(BGMT_PRESS_DOWN); /* Arrives before the mode request lands. */
        return;
    }
    if (shooting && ms==60 && ++shutter_ticks==4) {
        char src[80];
        snprintf(src,sizeof(src),"B:/DCIM/100CANON/IMG_%04d.JPG",card.file_number+1);
        assert(rename("staged.JPG",src)==0);
        if (!unavailable_dcim) {
            card.file_number++;
            qr=1;
            current_mode=17;
            busy=1;
        }
        return;
    }
    /* The camera review disappears before the live display stabilizes. */
    if (qr && active && page == DEVELOPING && ms == 100) {
        assert(!qr_presents && !disabled);
        qr=busy=0; current_mode=0;
    }
    if (ms!=40 || shooting || !active) return;
    if (awaiting_display) return;
    static unsigned info_ticks;
    if (!restarted && step==0 && info_ticks<104) {
        if (info_ticks==0) { assert(page==HOME); press(BGMT_INFO); }
        else if (info_ticks<=102) {
            assert(page==INFO_COVER);
            if (info_ticks==50) press(BGMT_UNPRESS_SET);
            if (info_ticks==102) press(BGMT_PRESS_SET);
        } else assert(page==HOME && !selection);
        ++info_ticks;
        return;
    }
    if (restarted) {
        switch (step++) {
        case 0: assert(page==HOME && !preview); press(BGMT_PRESS_UP); break;
        case 1: press(BGMT_PRESS_SET); break;
        case 2: assert(page==GALLERY && gallery_loaded && !colour_pixels() && photo_index==3 && photo_total==3 && !strcmp(print_name,"100_0102.JPG"));
                press(BGMT_PRESS_LEFT); break;
        case 3: assert(has_capture_date && !strcmp(photo_date,"2024:02:29 19:45:16"));
                assert(gallery_loaded && colour_pixels()>100 && photo_index==2 && photo_total==3 && !strcmp(print_name,"100_0101.JPG")); press(BGMT_PRESS_LEFT); break;
        case 4: assert(gallery_loaded && !strcmp(print_name,"P0001.JPG")); press(BGMT_PLAY); break;
        case 5: assert(page==HOME && !preview && !selection); press(BGMT_PRESS_SET); break;
        case 6: assert(page==FRAMES); press(BGMT_PRESS_LEFT); break;
        case 7: assert(frame==CR_NO_FRAME); press(BGMT_PRESS_SET); break;
        case 8: assert(page==RESULT && !strcmp(print_name,"100_0103.JPG")); press(BGMT_PLAY); break;
        case 9: assert(page==HOME); press(BGMT_PRESS_UP); break;
        case 10: press(BGMT_PRESS_SET); break;
        case 11: assert(page==GALLERY && gallery_loaded && !strcmp(print_name,"100_0103.JPG"));
                 frame=0; longjmp(finished,1);
        }
        return;
    }
    switch (step++) {
    case 0: assert(idle_ticks==100 && page==HOME); handle_key(BGMT_PRESS_DOWN); handle_key(BGMT_PRESS_DOWN); press(BGMT_PRESS_SET); break;
    case 1: assert(page==FRAMES && tone==3); press(BGMT_PRESS_SET); break;
    case 2: assert(has_capture_date && !strcmp(photo_date,"2024:02:29 19:45:16"));
            uint32_t expected_size;
            assert(!FIO_GetFileSize("DCIM/CANOROID/100_0101.JPG",&expected_size) && expected_size==print_bytes);
            assert(!FIO_GetFileSize(source_path,&expected_size) && expected_size==source_bytes);
            assert(page==RESULT && colour_pixels()>100 && photo_index==2 && photo_total==2 && !strcmp(print_name,"100_0101.JPG")); press(BGMT_PRESS_RIGHT); break;
    case 3: assert(compare_original && original_preview && colour_pixels()>100); press(BGMT_PLAY); break;
    case 4: assert(page==HOME && !preview);
            /* Quick Review ends while Home is up. The next task tick must retain the UI. */
            qr=busy=disabled=0; current_mode=0; break;
    case 5: assert(page==HOME); press(BGMT_PRESS_UP); press(BGMT_PRESS_UP); press(BGMT_PRESS_SET); break;
    case 6: if (open_requested || page==HOME) {step--; break;}
            assert(!lv && mono && page==FRAMES && display_available()); press(BGMT_PRESS_RIGHT); break;
    case 7: assert(frame==1 && !rename("second.JPG","staged.JPG"));
            shutter_ticks=0; unavailable_dcim=1; press(BGMT_PRESS_SET); break;
    case 8: assert(page==RESULT && colour_pixels()==0 && photo_index==3 && photo_total==3 && !strcmp(print_name,"100_0102.JPG"));
            handle_key(BGMT_PRESS_RIGHT);
            draw(100);
            assert(compare_original && colour_pixels()>100);
            assert(is_file(PRINT_DIR "/100_0101.JPG"));
            /* Same Canon number on a reused card. Keep all prior names. */
            char dst[48],tmp[48];
            assert(print_paths(dst,tmp) && !strcmp(dst, PRINT_DIR "/100A0102.JPG"));
            press(BGMT_MENU); break;
    case 9: assert(0 && "expected Canon handoff before next UI tick"); break;
    case 10: assert(!lv && page==HOME && active && selection==0 && CURRENT_GUI_MODE==GUIMODE_ML_MENU);
             press(BGMT_ISO); press(BGMT_Q_ALT_); press(BGMT_PRESS_DOWN); press(BGMT_PRESS_DOWN); press(BGMT_PRESS_DOWN); press(BGMT_PRESS_DOWN); press(BGMT_PRESS_SET); break;
    case 11: if (selection>=1 && selection<=3) {step--; break;} assert(page==HOME && selection==4); break;
    case 12: assert(page==GALLERY && gallery_loaded); press(BGMT_TRASH); break;
    case 13: assert(page==DELETE_PRINT && !delete_selected); press(BGMT_PRESS_SET); break;
    case 14: assert(page==GALLERY && is_file(PRINT_DIR "/100_0102.JPG"));
             press(BGMT_PLAY); break;
    case 15: assert(page==HOME); break;
    case 16: {
        static int idle_count;
        static unsigned count;
        if (!idle_count) count=presents;
        assert(count==presents);
        if (++idle_count<30) {step--; break;}
        front[40*720+38] ^= 0xff;
        break;
    }
    case 17: if (bmp_getpixel(38,40)!=back[40*720+38]) {step--; break;}
             assert(preview_pixels>=360*360);
             longjmp(finished,1);
    }
}
int main(int argc,char **argv)
{
    for (int i=0;i<512;i++) {
        LCD_Palette[3*i]=i;
        LCD_Palette[3*i+2]=registers[i]=0x03eb0000;
    }
    assert(!canoroid_owns_display());
    shooting=qr=1; assert(canoroid_owns_display());
    qr=0; assert(!canoroid_owns_display());
    shooting=0; active=1; assert(canoroid_owns_display());
    active=0;
    testing_guide=1;
    for (frame=0; frame<CR_FRAME_COUNT; ++frame) {
        memset(guide_pixels,0,sizeof(guide_pixels));
        memset(front,0xCC,sizeof(front));
        rail_buffer=NULL;
        guide();
        if (frame==CR_NO_FRAME) {
            assert(!rail_buffer && front[0]==0xCC && !guide_pixels[0][0]);
            continue;
        }
        const struct cr_frame *f=&cr_frames[frame];
        int left=120+480*f->inset/1080, right=120+480*(1080-f->inset)/1080;
        int top=480*f->top/1080, bottom=480*(f->top+f->height)/1080;
        for (int x=left; x<right; ++x)
            assert(guide_pixels[top][x]==COLOR_CYAN && guide_pixels[bottom-1][x]==COLOR_CYAN);
        for (int y=top; y<bottom; ++y)
            assert(guide_pixels[y][left]==COLOR_CYAN && guide_pixels[y][right-1]==COLOR_CYAN);
        assert(guide_pixels[(top+bottom)/2][360]==0);
        for (int x=0; x<720; ++x) {
            assert(guide_pixels[479][x]==0);
            assert(front[479*720+x]==0xCC);
        }
        int clear=0;
        for (int y=0; y<4; ++y)
            for (int x=0; x<4; ++x) {
                int pixel=front[y*720+x];
                assert(pixel==COLOR_EMPTY || pixel==COLOR_ALMOST_BLACK);
                clear+=pixel==COLOR_EMPTY;
            }
        assert(clear==5);
        assert(front[((top+bottom)/2)*720+360]==0xCC);
        assert(front[(top+5)*720+left-1]==mask_pixel(left-1,top+5));
        assert(front[(top+5)*720+right]==mask_pixel(right,top+5));
    }
    memset(front,0,sizeof(front));
    testing_guide=0;
    frame=0;
    /* Review Hold stays on QR until a keypress. It can be used once stable. */
    qr=1; current_mode=17; image_review_time=0xff; awaiting_display=1;
    claim_review_screen(); assert(!disabled && awaiting_display);
    sim_ms+=210; claim_review_screen(); assert(disabled && !awaiting_display);
    /* A verified result may arrive before timed QR ends. No partial result
     * may be presented until the replacement display is stable. */
    disabled=0; image_review_time=2; page=RESULT; reveal_pending=1;
    unsigned prior=presents;
    show_finished_page(); assert(presents==prior && reveal_pending && awaiting_display);
    qr=current_mode=0;
    show_finished_page(); sim_ms+=180; show_finished_page();
    assert(presents==prior && reveal_pending);
    sim_ms+=40; show_finished_page();
    assert(presents==prior+9 && !reveal_pending && !awaiting_display);
    page=HOME; disabled=0; photo_palette(0);
    format_file_time(951827696);
    assert(!strcmp(photo_date,"2000:02:29 12:34:56"));
    restarted=argc>1;
    if (restarted) {card.file_number=102; unavailable_dcim=1;}
    if (setjmp(finished)==0) canoroid_task(NULL);
    assert(developing_draws > 0);
    if (restarted) {
        /* The on-camera report names IMG_2144.JPG and a failed TMP create. */
        strcpy(source_path,"B:/DCIM/100CANON/IMG_2144.JPG");
        release_preview();
        tmp_denials=3;
        make_print();
        assert(page==FAILED && !tmp_denials && !strcmp(error,"Cannot create DCIM/CANOROID/100_2144.TMP"));
        draw(100);
        assert(!strcmp(displayed_error,error));
        assert(is_file(LEGACY_DIR "/E0001.TXT") && !is_file(PRINT_DIR "/100_2144.JPG"));
        tmp_denials=2;
        make_print();
        assert(page==RESULT && !tmp_denials && !strcmp(print_name,"100_2144.JPG"));
        assert(!is_file(PRINT_DIR "/100_2144.TMP"));
        release_preview();
        strcpy(source_path,"B:/DCIM/100CANON/IMG_2147.JPG");
        make_print();
        assert(page==RESULT && !strcmp(print_name,"100_2147.JPG"));
        /* Confirm deletion only for a selected framed JPEG. Walk the
         * gallery through deletion of its last entry. */
        browse_prints(-1);
        assert(gallery_loaded && is_file(PRINT_DIR "/100_2147.JPG"));
        handle_key(BGMT_TRASH);
        handle_key(BGMT_PRESS_RIGHT);
        handle_key(BGMT_PLAY);
        assert(page==GALLERY && gallery_loaded && is_file(PRINT_DIR "/100_2147.JPG"));
        deny_delete=1;
        handle_key(BGMT_TRASH);
        handle_key(BGMT_PRESS_RIGHT);
        handle_key(BGMT_PRESS_SET);
        assert(page==GALLERY && !gallery_loaded && strstr(error,"Could not delete"));
        assert(is_file(PRINT_DIR "/100_2147.JPG"));
        deny_delete=0;
        browse_prints(1);
        int deleted=0;
        while (gallery_loaded) {
            char path[48];
            snprintf(path,sizeof(path),"%s/%s",print_dirs[gallery_position % 2],print_name);
            handle_key(BGMT_TRASH);
            assert(page==DELETE_PRINT && !delete_selected);
            handle_key(BGMT_PRESS_RIGHT);
            handle_key(BGMT_PRESS_SET);
            assert(page==GALLERY && !is_file(path));
            deleted++;
        }
        assert(deleted==6 && strstr(error,"No saved prints") && is_file("B:/DCIM/100CANON/IMG_2147.JPG"));
        /* Exercise a genuine decoder error twice, including an unavailable
         * per-image log. Neither attempt may touch a completed photo. */
        strcpy(source_path,"B:/DCIM/100CANON/IMG_0199.JPG");
        release_preview();
        make_print();
        assert(page==FAILED && strstr(error,"JPEG") && is_file(LEGACY_DIR "/E0002.TXT"));
        refuse_report=1;
        make_print();
        assert(page==FAILED && is_file("ML/CANOROID.LOG"));
        assert(!is_file(PRINT_DIR "/100_0199.JPG") && !is_file(PRINT_DIR "/100_0199.TMP"));
    }
    release_preview();
    puts(restarted ? "Reboot, gallery deletion, cancellation, original preservation and failure logging passed"
                   : "Two framed shots, Canon menu reentry, queued presses and idle redraw checks passed");
}
