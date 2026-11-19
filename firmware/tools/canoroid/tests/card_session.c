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

enum { BGMT_MENU=1, BGMT_PLAY, BGMT_TRASH, BGMT_PRESS_HALFSHUTTER, BGMT_LV,
    BGMT_PRESS_UP, BGMT_PRESS_DOWN, BGMT_PRESS_LEFT, BGMT_PRESS_RIGHT, BGMT_PRESS_SET,
    BGMT_UNPRESS_UP, BGMT_UNPRESS_DOWN, BGMT_UNPRESS_LEFT, BGMT_UNPRESS_RIGHT, BGMT_UNPRESS_SET };
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
#define DISPLAY_IS_ON 1
#define get_halfshutter_pressed() 0
#define is_movie_mode() 0
#define liveview_display_idle() (!qr)
#define job_state_ready_to_take_pic() (!busy)
#define canon_gui_front_buffer_disabled() disabled
#define canon_gui_disable_front_buffer() (disabled=1)
#define canon_gui_enable_front_buffer(x) (disabled=0)
#define SetGUIRequestMode(mode) (current_mode=(mode))
#define fake_simple_button(k) (lv=1)
#define BMP_LOCK(x) {x}
#define bmp_fill(...) ((void)0)
#define bmp_draw_rect(...) ((void)0)
#define bmp_draw_rect_chamfer(...) ((void)0)
static char displayed_error[100];
static int developing_draws;
static void record_label(int font, int x, int y, const char *fmt, const char *label) {
    if (!strcmp(label, "Making your print")) developing_draws++;
    if (x == 48 && y >= 171 && y <= 264) {
        if (y == 171) displayed_error[0] = 0;
        assert(strlen(displayed_error) + strlen(label) < sizeof(displayed_error));
        strcat(displayed_error, label);
    }
}
#define bmp_printf record_label
static uint8_t back[720 * 480], front[720 * 480];
static unsigned sim_ms, presents, preview_pixels;
static int drawing_back;
#define COLOR_ORANGE 42
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
}
static void bmp_putpixel_fast(uint8_t *vram, int x, int y, uint8_t color) {
    assert(drawing_back && vram==back && x>=38 && x<398 && y>=62 && y<422);
    vram[y*720+x]=color;
    preview_pixels++;
}
#define bmp_on() ((void)0)
#define redraw() ((void)0)
#define get_yuv422_vram() ((void)0)
#define TASK_CREATE(...)
enum {FONT_SMALL, FONT_MED, FONT_LARGE, FONT_MED_LARGE, COLOR_CYAN, COLOR_ALMOST_WHITE, COLOR_YELLOW};
#define COLOR_GRAY(n) (n)
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
struct fio_file {int mode; char name[256];};
struct fio_dirent {DIR *dir;};
static int FIO_FindNextEx(struct fio_dirent *it,struct fio_file *file) {
    struct dirent *e=readdir(it->dir);
    if (!e) return -1;
    struct stat st;
    assert(!fstatat(dirfd(it->dir),e->d_name,&st,0));
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
static void msleep(int ms)
{
    sim_ms += ms;
    if (++total_ticks == 2500) { fprintf(stderr, "Timeout step %d page %d active %d gui %d/%d shot %d now %u hash %u/%u frame %u\n",step,page,active,current_mode,gui_mode,shooting,sim_ms,screen_signature(front),last_signature,presents); abort(); }
    if (!active && !opening && !open_requested && !shooting && !restarted && step==9 && ms==40) {
        step++;
        assert(!disabled && current_mode==GUIMODE_MENU);
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
    if (ms!=40 || shooting || !active) return;
    if (restarted) {
        switch (step++) {
        case 0: assert(page==HOME && !preview); press(BGMT_PRESS_UP); break;
        case 1: press(BGMT_PRESS_SET); break;
        case 2: assert(page==GALLERY && gallery_loaded && !strcmp(print_name,"100_0102.JPG"));
                press(BGMT_PRESS_LEFT); break;
        case 3: assert(gallery_loaded && !strcmp(print_name,"100_0101.JPG")); press(BGMT_PRESS_LEFT); break;
        case 4: assert(gallery_loaded && !strcmp(print_name,"P0001.JPG")); press(BGMT_PLAY); break;
        case 5: assert(page==HOME && !preview && !selection); press(BGMT_PRESS_SET); break;
        case 6: assert(page==FRAMES); press(BGMT_PRESS_SET); break;
        case 7: assert(page==RESULT && !strcmp(print_name,"100_0103.JPG")); longjmp(finished,1);
        }
        return;
    }
    switch (step++) {
    case 0: assert(page==HOME); press(BGMT_PRESS_SET); break;
    case 1: assert(page==FRAMES); press(BGMT_PRESS_SET); break;
    case 2: assert(page==RESULT && !strcmp(print_name,"100_0101.JPG")); press(BGMT_PRESS_RIGHT); break;
    case 3: assert(compare_original && original_preview); press(BGMT_PLAY); break;
    case 4: assert(page==HOME && !preview);
            /* Quick Review ends while Home is up. The next task tick must retain the UI. */
            qr=busy=disabled=0; current_mode=0; break;
    case 5: assert(page==HOME); press(BGMT_PRESS_SET); break;
    case 6: if (open_requested) {step--; break;}
            assert(page==FRAMES && display_available()); press(BGMT_PRESS_RIGHT); break;
    case 7: assert(frame==1 && !rename("second.JPG","staged.JPG"));
            shutter_ticks=0; unavailable_dcim=1; press(BGMT_PRESS_SET); break;
    case 8: assert(page==RESULT && !strcmp(print_name,"100_0102.JPG"));
            assert(is_file(PRINT_DIR "/100_0101.JPG"));
            /* Same Canon number on a reused card. Keep all prior names. */
            char dst[48],tmp[48];
            assert(print_paths(dst,tmp) && !strcmp(dst, PRINT_DIR "/100A0102.JPG"));
            press(BGMT_MENU); break;
    case 9: assert(0 && "expected Canon handoff before next UI tick"); break;
    case 10: assert(page==HOME && active && selection==1 && CURRENT_GUI_MODE==GUIMODE_ML_MENU);
             press(BGMT_PRESS_DOWN); press(BGMT_PRESS_SET); break;
    case 11: assert(page==HOME && selection==2); break;
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
        assert(page==FAILED && !tmp_denials && !strcmp(error,"Cannot create ML/CANOROID/100_2144.TMP"));
        draw(100);
        assert(!strcmp(displayed_error,error));
        assert(is_file(PRINT_DIR "/E0001.TXT") && !is_file(PRINT_DIR "/100_2144.JPG"));
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
            snprintf(path,sizeof(path),PRINT_DIR "/%s",print_name);
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
        assert(page==FAILED && strstr(error,"JPEG") && is_file(PRINT_DIR "/E0002.TXT"));
        refuse_report=1;
        make_print();
        assert(page==FAILED && is_file("ML/CANOROID.LOG"));
        assert(!is_file(PRINT_DIR "/100_0199.JPG") && !is_file(PRINT_DIR "/100_0199.TMP"));
    }
    release_preview();
    puts(restarted ? "Reboot, gallery deletion, cancellation, original preservation and failure logging passed"
                   : "Two framed shots, Canon menu reentry, queued presses and idle redraw checks passed");
}
