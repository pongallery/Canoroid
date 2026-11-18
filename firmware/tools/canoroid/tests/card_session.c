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
#define bmp_printf(...) ((void)0)
#define bmp_putpixel_fast(...) ((void)0)
#define bmp_vram() NULL
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
static FILE *FIO_CreateFile(const char *p) {
    if (refuse_report && strstr(p, "/E") && strstr(p, ".TXT")) return NULL;
    return fopen(p,"wb");
}
#define FIO_CloseFile fclose
#define FIO_ReadFile(f,p,n) ((int)fread(p,1,n,f))
#define FIO_WriteFile(f,p,n) ((int)fwrite(p,1,n,f))
#define FIO_SeekSkipFile fseek
#define FIO_RenameFile rename
#define FIO_RemoveFile unlink
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
#include "../../../src/canoroid/camera.c"

static int step, restarted, shutter_ticks, total_ticks;
static jmp_buf finished;
static void press(int button) {assert(canoroid_handle_key(button)==0);}
static void msleep(int ms)
{
    assert(++total_ticks<2500);
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
    case 4: assert(page==HOME && retained_page && !preview);
            /* Quick Review ends while Home is up. The next task tick must retain the UI. */
            qr=busy=disabled=0; current_mode=0; break;
    case 5: assert(page==HOME && display_available()); press(BGMT_PRESS_SET); break;
    case 6: assert(page==FRAMES); press(BGMT_PRESS_RIGHT); break;
    case 7: assert(frame==1 && !rename("second.JPG","staged.JPG"));
            shutter_ticks=0; unavailable_dcim=1; press(BGMT_PRESS_SET); break;
    case 8: assert(page==RESULT && !strcmp(print_name,"100_0102.JPG"));
            assert(is_file(PRINT_DIR "/100_0101.JPG"));
            /* Same Canon number on a reused card. Keep all prior names. */
            char dst[48],tmp[48];
            assert(print_paths(dst,tmp) && !strcmp(dst, PRINT_DIR "/100A0102.JPG"));
            longjmp(finished,1);
    }
}
int main(int argc,char **argv)
{
    restarted=argc>1;
    if (restarted) {card.file_number=102; unavailable_dcim=1;}
    if (setjmp(finished)==0) canoroid_task(NULL);
    if (restarted) {
        /* Exercise a genuine decoder error twice, including an unavailable
         * per-image log. Neither attempt may touch a completed photo. */
        strcpy(source_path,"B:/DCIM/100CANON/IMG_0199.JPG");
        release_preview();
        make_print();
        assert(page==FAILED && strstr(error,"JPEG") && is_file(PRINT_DIR "/E0001.TXT"));
        refuse_report=1;
        make_print();
        assert(page==FAILED && is_file("ML/CANOROID.LOG"));
        assert(!is_file(PRINT_DIR "/100_0199.JPG") && !is_file(PRINT_DIR "/100_0199.TMP"));
    }
    release_preview();
    puts(restarted ? "Card reinsert, legacy gallery, fresh capture and failure logging passed"
                   : "Two framed shots via Home across Quick Review passed");
}
