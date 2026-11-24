/* SPDX-License-Identifier: GPL-2.0-only */
/* Native adaptation of the supplied DinoFrame Run v0.2. Coordinates use
 * its 360 x 240 playfield. Physics runs at 100 Hz with integer arithmetic. */
enum run_state { RUN_TITLE, RUN_PLAYING, RUN_PAUSED, RUN_OVER };
enum hurdle_kind { FILM_ROLL, TRIPOD, FLYING_PRINT };
struct hurdle { int x100, kind, used; };
static struct {
    enum run_state state;
    struct hurdle obstacles[4];
    unsigned distance100, rng, last_ms, remainder, best;
    int y100, velocity, spawn, buffered, held_down;
} run;
#define DINO_BEST LEGACY_DIR "/DINO.BST"

static unsigned run_score(void) { return run.distance100/1000; }

static void run_record(void)
{
    unsigned score=run_score();
    if (score<=run.best) return;
    run.best=score;
    FILE *f=FIO_CreateFile(DINO_BEST);
    if (f) {
        FIO_WriteFile(f,&score,sizeof(score));
        FIO_CloseFile(f);
    }
}

static void run_open(void)
{
    FILE *f=FIO_OpenFile(DINO_BEST,O_RDONLY|O_SYNC);
    if (f) {
        unsigned *n=fio_malloc(sizeof(*n));
        if (n) {
            if (FIO_ReadFile(f,n,sizeof(*n))==sizeof(*n) && *n<=4294967 && *n>run.best)
                run.best=*n;
            fio_free(n);
        }
        FIO_CloseFile(f);
    }
    run.state=RUN_TITLE;
    run.y100=20000;
    run.distance100=run.held_down=0;
    memset(run.obstacles,0,sizeof(run.obstacles));
    run.rng=get_ms_clock();
}

static unsigned run_random(void)
{
    run.rng=1664525u*run.rng+1013904223u;
    return run.rng>>16;
}

static void run_key(int key)
{
    if (key==BGMT_UNPRESS_DOWN) { run.held_down=0; return; }
    if (key==BGMT_PRESS_DOWN && run.state==RUN_PLAYING) { run.held_down=1; return; }
    if (key==BGMT_Q || key==BGMT_Q_ALT_) {
        if (run.state==RUN_PLAYING) { run.state=RUN_PAUSED; run.held_down=0; }
        else if (run.state==RUN_PAUSED) { run.state=RUN_PLAYING; run.last_ms=get_ms_clock(); }
        return;
    }
    if (key!=BGMT_PRESS_SET && key!=BGMT_PRESS_UP) return;
    if (run.state==RUN_PLAYING) {
        if (run.y100==20000 && !run.velocity) run.velocity=-3100;
        else run.buffered=12;
        return;
    }
    if (run.state!=RUN_PAUSED) {
        memset(run.obstacles,0,sizeof(run.obstacles));
        run.distance100=run.velocity=run.buffered=run.held_down=0;
        run.y100=20000;
        run.spawn=200;
    }
    run.state=RUN_PLAYING;
    run.last_ms=get_ms_clock();
    run.remainder=0;
}

static int run_duck(void) { return run.held_down && run.y100==20000 && !run.velocity; }

/* Only solid sprite parts collide, including the two thin tripod legs. */
static int run_overlap(int x,int y,int w,int h)
{
    static const signed char stand[][4]={{10,1,8,7},{18,6,3,2},{7,11,8,10},{8,25,2,3},{15,25,2,3}};
    static const signed char crouch[][4]={{3,6,17,6},{18,1,8,7},{5,14,2,1}};
    int duck=run_duck(), top=run.y100/100-(duck?16:30);
    const signed char (*parts)[4]=duck?crouch:stand;
    for (int i=0;i<(duck?3:5);i++) {
        int px=35+parts[i][0], py=top+parts[i][1];
        if (x<px+parts[i][2] && px<x+w && y<py+parts[i][3] && py<y+h) return 1;
    }
    return 0;
}

static int run_hit(int x,int kind)
{
    if (x>62 || x+19<35) return 0;
    if (kind==FILM_ROLL) return run_overlap(x+2,178,10,21);
    if (kind==FLYING_PRINT) return run_overlap(x+1,168,15,12);
    if (run_overlap(x+8,171,2,4)) return 1;
    for (int y=7;y<29;y+=2)
        if (run_overlap(x+7-y/5,170+y,1,1) || run_overlap(x+9+y/5,170+y,1,1)) return 1;
    return 0;
}

static void run_step(void)
{
    unsigned speed=90+MIN(run_score()*9/100,90u);
    if (run.distance100<=0xffffffffu-speed) run.distance100+=speed;
    if (run.velocity || run.y100<20000) {
        run.velocity+=85;
        run.y100+=run.velocity/10;
        if (run.y100>=20000) {
            run.y100=20000;
            run.velocity=run.buffered ? -3100 : 0;
            run.buffered=0;
        }
    }
    if (run.buffered) run.buffered--;
    if (--run.spawn<=0) {
        for (int i=0;i<4;i++) if (!run.obstacles[i].used) {
            int kind=run_score()>80 && run_random()%10<3 ? FLYING_PRINT : (int)(run_random()%2);
            run.obstacles[i]=(struct hurdle){38000,kind,1};
            break;
        }
        run.spawn=150+run_random()%71;
    }
    for (int i=0;i<4;i++) {
        struct hurdle *o=&run.obstacles[i];
        if (!o->used) continue;
        o->x100-=(int)speed;
        if (o->x100 < -2000) o->used=0;
        else if (run_hit(o->x100/100,o->kind)) {
            run.state=RUN_OVER;
            run.held_down=0;
            run_record();
            break;
        }
    }
}

static void run_update(void)
{
    unsigned now=get_ms_clock(), elapsed=now-run.last_ms;
    run.last_ms=now;
    run.remainder+=MIN(elapsed,100u);
    while (run.remainder>=10 && run.state==RUN_PLAYING) {
        run.remainder-=10;
        run_step();
    }
}

/* Clip before passing coordinates to the bitmap driver. */
static void sprite(int color,int x,int y,int w,int h)
{
    if (x<0) { w+=x; x=0; }
    w=MIN(w,360-x);
    if (w>0) bmp_fill(color,x*2,y*2,w*2,h*2);
}

static void run_draw(void)
{
    unsigned distance=run.distance100/100;
    int night=(run_score()/250)%2, sky=night?UI_BG:UI_PANEL;
    bmp_fill(sky,0,0,720,480);
    char score[48];
    snprintf(score,sizeof(score),"Score %d    Best %d",(int)run_score(),(int)run.best);
    text(FONT(FONT_MED_LARGE,UI_CYAN,sky),24,14,score);
    for (int i=0;i<5;i++) {
        int x=(i*83+430-(int)(distance/6%430))%430-30;
        sprite(night?UI_TEXT:UI_MUTED,x,48+i*8,night?2:22,2);
    }
    sprite(UI_MINT,0,200,360,3);
    for (int i=0;i<15;i++) sprite(UI_MUTED,(i*27+405-(int)(distance%405))%405-5,206,4,1);
    int duck=run_duck(), y=run.y100/100-(duck?16:30);
    int body=UI_MINT;
    if (duck) {
        sprite(body,36,y+5,21,9); sprite(body,52,y,10,10);
        sprite(UI_TEXT,39,y+13,4,3); sprite(UI_BG,56,y+2,2,2);
    } else {
        sprite(body,44,y,10,10); sprite(body,52,y+5,5,4);
        sprite(body,40,y+10,12,14); sprite(body,35,y+11,6,10);
        int stride=(distance/8)%2;
        sprite(body,42,y+24,4,stride?4:6); sprite(body,49,y+24,4,stride?6:4);
        sprite(UI_BG,50,y+2,2,2);
    }
    sprite(UI_TEXT,37,y+8,7,7); sprite(UI_BG,39,y+10,3,3);
    for (int i=0;i<4;i++) {
        struct hurdle *o=&run.obstacles[i];
        if (!o->used) continue;
        int x=o->x100/100;
        if (o->kind==FILM_ROLL) {
            sprite(COLOR_YELLOW,x,176,14,24); sprite(UI_BRAND,x+2,181,10,12);
        } else if (o->kind==FLYING_PRINT) {
            sprite(UI_TEXT,x,167,17,14); sprite(UI_CYAN,x+2,169,13,7);
        } else {
            sprite(UI_BRAND,x+5,170,9,6);
            for (int j=5;j<30;j+=2) {
                sprite(UI_TEXT,x+7-j/5,170+j,2,2); sprite(UI_TEXT,x+9+j/5,170+j,2,2);
            }
        }
    }
    if (run.state!=RUN_PLAYING) {
        rounded(UI_EDGE,56,120,608,160);
        const char *title=run.state==RUN_TITLE?"DinoFrame Run":run.state==RUN_PAUSED?"Paused":"Game over";
        text(FONT(FONT_LARGE,UI_TEXT,UI_EDGE)|FONT_ALIGN_CENTER,360,132,title);
        text(FONT(FONT_MED,UI_TEXT,UI_EDGE)|FONT_ALIGN_CENTER,360,184,
             "Jump over gear. Duck under photos.");
        text(FONT(FONT_MED_LARGE,UI_CYAN,UI_EDGE)|FONT_ALIGN_CENTER,360,228,
             run.state==RUN_TITLE?"SET to start":run.state==RUN_PAUSED?"SET to resume":"SET to retry");
    }
    bmp_fill(UI_BG,0,426,720,54);
    text(FONT(FONT_MED,UI_TEXT,UI_BG)|FONT_ALIGN_CENTER,360,442,"SET Jump   Hold DOWN Duck   Q Pause   PLAY Home");
}
