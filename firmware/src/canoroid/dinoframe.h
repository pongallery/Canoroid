/* SPDX-License-Identifier: GPL-2.0-only */
/* Native adaptation of the supplied DinoFrame Run v0.2. Coordinates use
 * its 360 x 240 playfield. Physics runs at 100 Hz with integer arithmetic. */
enum run_state { RUN_TITLE, RUN_PLAYING, RUN_PAUSED, RUN_OVER };
enum hurdle_kind { FILM_ROLL, FLYING_CAMERA };
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
        if (run.y100==20000 && !run.velocity) run.velocity=-3350;
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

/* Indexed pixel art. The player and obstacle share these row masks with
 * collision detection. All coordinates below are logical game pixels. */
struct run_art { const char *const *rows; int height; };
#define RUN_ART(rows) {rows,(int)(sizeof(rows)/sizeof(rows[0]))}
static const char *const trike_rows[]={
    "                      bb",
    "                    bbBBb      w",
    "                   bBBHHBb    ww",
    "                  bBBHHHBBb  ww",
    "                 bBBHHBBBBbbww  w",
    "                  bBBBBBHHBww  ww",
    "       yy       bbbBBBBBHHBBbbww",
    "    yyyyYy    bbBBbyyBBBBBBBBBb",
    "   yy    yyyybbBHHHbyyBBBddwBBb   w",
    "        yyyybbBHHBBBbyyBBddwBBBbbww",
    "           bBBHHBBBBByyyBBBBBBBBBwb",
    "     bb  bbBBBBBBBBBBByyBBBBBBBBBBb",
    " bb  bBbbBBBBBBBBBBBBBByyBBBBbbBBb",
    "  bbbbBBBBBBBBBBBBBBBBBbbHHHbbbb",
    "    bbBBBBBBBBBBBBBBBBBBbbbbb",
    "      bbbbBBBBBBBBBBBBBb",
    "          bbbbBBBBBBBBb",
    "             bbbbbbbbb"
};
static const char *const duck_rows[]={
    "                     bb       w",
    "                    bBBbb    ww",
    "       yyyy        bBHHBBbb ww  w",
    "   yyyy    yyyybbbbBBBHBBBbww  ww",
    "         bbBBHHBBBBByyBBBBBbbBww",
    "  bbb bbbBBHHHBBBBBBByyBBdwBBBBbb w",
    "   bbBBBBBBBBBBBBBBBBByyBBBBBBBBbww",
    "      bbBBBBBBBBBBBBBBByyBBBBBBBBBb",
    "        bbbbBBBBBBBBBBBbbHHHBBBBbb",
    "           bbbbbbbbbbbbbbbbbbbb"
};
static const char *const film_rows[]={
    "    mmmmm", "   mwwwwmm", " mmmmmmmmmmm", " mwwwwwwwwwm",
    "  bBBBBBBBb", "  bBBBBBBBb", "  bHHHHHHHb", "  bwwwwwwwb",
    "  bwBBwBBwbffff", "  bwBBwBBwbfddf", "  bwwwwwwwbffff",
    "  bBBBBBBBbfddf", "  bBBBBBBBbffff", "  bHHHHHHHb",
    "  bBBBBBBBb", " mwwwwwwwwwm", " mmmmmmmmmmm"
};
static const char *const camera_rows[]={
    "  mmm    mmmmm", "  www   mwwwwwm", " mwwwwwwwBBBBBwwwwwm",
    " mwwwwwwwwwwwwwwHHwm", " mdddmmmddmmmddddddm",
    " mdddmwwmmwwwmdddddm", " mddmwwmBBBBBwmddddm",
    " mddmwmBBddHBmwmdddm", " mddmwmBddddBmwmdddm",
    " mddmwmBddddBmwmdddm", " mddmwwmBBBBBwmddddm",
    " mdddmwwmmmmmwmddddm", " mmmmmmmwwwwwmmmmmmm"
};
static const struct run_art trike=RUN_ART(trike_rows), low_trike=RUN_ART(duck_rows);
static const struct run_art hurdles[]={RUN_ART(film_rows),RUN_ART(camera_rows)};
static int hurdle_top(int kind) { return kind==FILM_ROLL ? 200-hurdles[kind].height : 174; }
static int trike_top(void) { return run.y100/100-(run_duck()?low_trike.height+2:trike.height+4); }

static int run_hit(int x,int kind)
{
    if (x>72 || x+20<=35) return 0;
    const struct run_art *player=run_duck()?&low_trike:&trike, *other=&hurdles[kind];
    int top=trike_top(), oy=hurdle_top(kind);
    for (int i=0;i<player->height;i++) {
        int row=top+i-oy;
        if (row<0 || row>=other->height) continue;
        const char *p=player->rows[i], *o=other->rows[row];
        int left=MAX(x,35), right=MIN(35+(int)strlen(p),x+(int)strlen(o));
        for (int px=left;px<right;px++)
            if (p[px-35]!=' ' && o[px-x]!=' ') return 1;
    }
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
            run.velocity=run.buffered ? -3350 : 0;
            run.buffered=0;
        }
    }
    if (run.buffered) run.buffered--;
    if (--run.spawn<=0) {
        for (int i=0;i<4;i++) if (!run.obstacles[i].used) {
            int kind=run_score()>20 && run_random()%3==0 ? FLYING_CAMERA : FILM_ROLL;
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

/* Slots in the existing 6 x 7 x 4 preview palette. */
enum { TRIKE_BLUE=CUBE_BASE+28+4*4+3, TRIKE_LIGHT=CUBE_BASE+28*3+4*5+3,
       TRIKE_SHADOW=CUBE_BASE+4*2+2 };

static void paint_run_art(const struct run_art *art,int x,int y)
{
    for (int i=0;i<art->height;i++) {
        const char *row=art->rows[i];
        for (int start=0;row[start];) {
            char ink=row[start];
            int end=start+1;
            while (row[end]==ink) end++;
            if (ink!=' ') {
                int color=UI_BG;
                switch (ink) {
                case 'b': color=TRIKE_SHADOW; break;
                case 'B': color=TRIKE_BLUE; break;
                case 'H': color=TRIKE_LIGHT; break;
                case 'w': color=UI_TEXT; break;
                case 'y': case 'Y': color=COLOR_YELLOW; break;
                case 'f': color=UI_BRAND; break;
                case 'm': color=UI_MUTED; break;
                }
                sprite(color,x+start,y+i,end-start,1);
            }
            start=end;
        }
    }
}

static void run_draw(void)
{
    unsigned distance=run.distance100/100;
    int sky=UI_BG;
    bmp_fill(sky,0,0,720,480);
    char score[48];
    snprintf(score,sizeof(score),"Score %d    Best %d",(int)run_score(),(int)run.best);
    text(FONT(FONT_MED_LARGE,UI_CYAN,sky),24,14,score);
    /* Crescent, stars and a distant line of pines. */
    disc(COLOR_YELLOW,638,91,16);
    disc(sky,630,84,14);
    for (int i=0;i<15;i++) {
        int x=(i*73+390-(int)(distance/10%390))%390-12;
        sprite(i%3?UI_MUTED:UI_TEXT,x,47+i*27%78,2,2);
    }
    for (int i=0;i<14;i++) {
        int x=(i*31+434-(int)(distance/5%434))%434-28;
        int height=8+i*7%14;
        for (int w=2;w<=height;w+=3)
            sprite(UI_PANEL,x+height/2-w/2,198-height+w,w,3);
    }
    sprite(UI_MINT,0,200,360,3);
    for (int i=0;i<15;i++) sprite(UI_MUTED,(i*27+405-(int)(distance%405))%405-5,206,4,1);
    int duck=run_duck(), bottom=run.y100/100;
    int stride=run.velocity?2:(int)(distance/8%2)*3;
    for (int i=0;i<4;i++) {
        int foot=45+i*4+(i%2?stride:-stride);
        int lift=duck?0:(i%2?stride:3-stride);
        sprite(i%2?TRIKE_BLUE:TRIKE_SHADOW,foot,bottom-(duck?4:8),4,(duck?4:8)-lift);
        sprite(UI_TEXT,foot+1,bottom-lift-1,3,1);
    }
    paint_run_art(duck?&low_trike:&trike,35,trike_top());
    for (int i=0;i<4;i++) {
        struct hurdle *o=&run.obstacles[i];
        if (o->used) paint_run_art(&hurdles[o->kind],o->x100/100,hurdle_top(o->kind));
    }
    if (run.state!=RUN_PLAYING) {
        rounded(UI_EDGE,56,120,608,160);
        const char *title=run.state==RUN_TITLE?"Blue Trike":run.state==RUN_PAUSED?"Paused":"Game over";
        text(FONT(FONT_LARGE,UI_TEXT,UI_EDGE)|FONT_ALIGN_CENTER,360,132,title);
        text(FONT(FONT_MED,UI_TEXT,UI_EDGE)|FONT_ALIGN_CENTER,360,184,
             "Jump over film. Duck under cameras.");
        text(FONT(FONT_MED_LARGE,UI_CYAN,UI_EDGE)|FONT_ALIGN_CENTER,360,228,
             run.state==RUN_TITLE?"SET to start":run.state==RUN_PAUSED?"SET to resume":"SET to retry");
    }
    bmp_fill(UI_BG,0,426,720,54);
    text(FONT(FONT_MED,UI_TEXT,UI_BG)|FONT_ALIGN_CENTER,360,442,"SET Jump   Hold DOWN Duck   Q Pause   PLAY Home");
}
