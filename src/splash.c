/*
 * splash.c — metastreamer --splash: the boot screen. Started by rcS before anything else; draws the status screen
 * in its STARTING form on the console framebuffer (/dev/fb0, whatever mode the console has) with the loader
 * running and the boot's progress: rcS writes "DONE TOTAL WHAT" to /run/boot.step before each init script. metastreamer stops it (/run/splash.pid) when it takes
 * over the display, which shows the same layout.
 */
#include "splash.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "conf.h"
#include "screen.h"

#define STEP_FILE "/run/boot.step"
#define LIMIT_S 60   /* never outlive the boot by much */
#define MAXFB 4096

static int fd = -1;
static uint8_t *fb; static size_t fb_len;
static int fw, fh, bpp, stride;
static char fb_name[32];

static void fb_close(void) {
    if (fb) munmap(fb, fb_len);
    if (fd >= 0) close(fd);
    fb = NULL; fd = -1;
}

static void read_name(char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen("/sys/class/graphics/fb0/name", "r");
    if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

static int fb_open(void) {
    fb_close();
    read_name(fb_name, sizeof(fb_name));
    if ((fd = open("/dev/fb0", O_RDWR | O_CLOEXEC)) < 0) return -1;
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) { fb_close(); return -1; }
    fw = v.xres; fh = v.yres; bpp = v.bits_per_pixel; stride = f.line_length;
    if ((bpp != 16 && bpp != 32) || !fw || !fh || fw > MAXFB || fh > MAXFB) { fb_close(); return -1; }
    fb_len = (size_t)stride * v.yres_virtual;
    fb = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { fb = NULL; fb_close(); return -1; }
    return 0;
}

/* Screen -> framebuffer, scaled by area averaging (downscaled text stays smooth). The ARM11 has no divide
 * instruction, so the boxes come from tables, and only what is drawn is scaled: the rest of the screen is black. */
static uint16_t xa[MAXFB], xb[MAXFB], ya[MAXFB], yb[MAXFB];
static int tables_w, tables_h;

static void tables(void) {
    if (tables_w == fw && tables_h == fh) return;
    for (int x = 0; x < fw; x++) {
        xa[x] = x * SCREEN_W / fw; xb[x] = ((x + 1) * SCREEN_W + fw - 1) / fw;
        if (xb[x] <= xa[x]) xb[x] = xa[x] + 1;
    }
    for (int y = 0; y < fh; y++) {
        ya[y] = y * SCREEN_H / fh; yb[y] = ((y + 1) * SCREEN_H + fh - 1) / fh;
        if (yb[y] <= ya[y]) yb[y] = ya[y] + 1;
    }
    tables_w = fw; tables_h = fh;
}

static void copy_rect(int sx, int sy, int sw, int sh) {
    const uint32_t *px = screen_pixels();
    const uint16_t *rx0, *rx1;
    screen_extents(&rx0, &rx1);
    static const uint16_t inv[17] = {0, 65535, 32768, 21845, 16384, 13107, 10923, 9362, 8192, 7282, 6554, 5958, 5461,
                                     5041, 4681, 4369, 4096};
    tables();
    int x0 = sx * fw / SCREEN_W, x1 = ((sx + sw) * fw + SCREEN_W - 1) / SCREEN_W;
    int y0 = sy * fh / SCREEN_H, y1 = ((sy + sh) * fh + SCREEN_H - 1) / SCREEN_H;
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    int bytes = bpp / 8;
    for (int y = y0; y < y1; y++) {
        uint8_t *row = fb + (size_t)y * stride;
        int c0 = SCREEN_W, c1 = 0;   /* drawn columns in the source rows of this line */
        for (int j = ya[y]; j < yb[y]; j++)
            if (rx0[j] < rx1[j]) { if (rx0[j] < c0) c0 = rx0[j]; if (rx1[j] > c1) c1 = rx1[j]; }
        int d0 = x1, d1 = x0;        /* ... as framebuffer columns, within the rectangle */
        if (c0 < c1) {
            d0 = c0 * fw / SCREEN_W; d1 = (c1 * fw + SCREEN_W - 1) / SCREEN_W;
            if (d0 < x0) d0 = x0;
            if (d1 > x1) d1 = x1;
        }
        if (d0 >= d1) { memset(row + x0 * bytes, 0, (x1 - x0) * bytes); continue; }
        memset(row + x0 * bytes, 0, (d0 - x0) * bytes);
        memset(row + d1 * bytes, 0, (x1 - d1) * bytes);
        for (int x = d0; x < d1; x++) {
            unsigned r = 0, g = 0, b = 0, n = (yb[y] - ya[y]) * (xb[x] - xa[x]);
            for (int j = ya[y]; j < yb[y]; j++)
                for (int i = xa[x]; i < xb[x]; i++) {
                    uint32_t p = px[j * SCREEN_W + i];
                    r += p >> 16 & 255; g += p >> 8 & 255; b += p & 255;
                }
            if (n > 1 && n <= 16) { r = r * inv[n] >> 16; g = g * inv[n] >> 16; b = b * inv[n] >> 16; }
            else if (n > 16) { r /= n; g /= n; b /= n; }
            if (bpp == 32) ((uint32_t *)row)[x] = r << 16 | g << 8 | b;
            else ((uint16_t *)row)[x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
        }
    }
}

/* the boot's progress from rcS: "DONE TOTAL WHAT"; 1 if it changed */
static int read_step(ScreenState *s, char *seen, size_t n) {
    FILE *f = fopen(STEP_FILE, "r");
    char line[64] = "";
    int done = 0, total = 0, off = 0;
    if (f) { if (fgets(line, sizeof(line), f)) line[strcspn(line, "\n")] = 0; fclose(f); }
    if (!strcmp(line, seen)) return 0;
    snprintf(seen, n, "%s", line);
    if (sscanf(line, "%d %d %n", &done, &total, &off) != 2) return 0;
    s->boot_done = done; s->boot_total = total;
    snprintf(s->boot_step, sizeof(s->boot_step), "%s", line + off);
    return 1;
}

static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int splash_main(void) {
    double t0 = mono();
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    conf_load();
    ScreenState s;
    memset(&s, 0, sizeof(s));
    s.starting = 1;
    s.stage = STAGE_USB;
    s.wifi = conf_power() == POWER_MIN ? WIFI_OFF : WIFI_SEARCHING;
    char seen[64] = "";
    read_step(&s, seen, sizeof(seen));   /* already there: one drawing, not two */
    screen_update(&s);
    double check = 0;
    int full = 1, hint = 0, last[SCREEN_ANIMS];
    while (!stop && mono() - t0 < LIMIT_S) {
        double t = mono();
        if (t - check >= 0.1) {   /* the boot step, and the console framebuffer replaced (simplefb -> vc4) */
            check = t;
            if (read_step(&s, seen, sizeof(seen))) { screen_update(&s); hint = 1; }
            char name[32];
            read_name(name, sizeof(name));
            if (!fb || strcmp(name, fb_name)) { if (fb_open() == 0) full = 1; }
        }
        if (!fb) { usleep(50000); continue; }
        if (full) {
            copy_rect(0, 0, SCREEN_W, SCREEN_H); full = hint = 0;
            for (int a = 0; a < SCREEN_ANIMS; a++) last[a] = -1;
            printf("[splash] %s %dx%d %d bpp: screen in %.0f ms\n", fb_name, fw, fh, bpp, (mono() - t0) * 1000);
        }
        if (hint) {   /* the line under the title and the progress (the loader in it starts over) */
            copy_rect(0, 520, SCREEN_W, 200); hint = 0;
            for (int a = 0; a < SCREEN_ANIMS; a++) last[a] = -1;
        }   /* the line under the title and the progress */
        /* the animations, by the clock (the console framebuffer has no vblank wait): sleep until the next frame */
        double ms = (mono() - t0) * 1000, wait = 100;
        for (int a = 0; a < SCREEN_ANIMS; a++) {
            int n, fms, x, y, w, h;
            if (!screen_anim_info(a, &n, &fms)) continue;
            long f = (long)(ms / fms);
            int k = (int)(f % n);
            if (k != last[a] && screen_anim_frame(a, k, &x, &y, &w, &h)) { copy_rect(x, y, w, h); last[a] = k; }
            if ((f + 1) * fms - ms < wait) wait = (f + 1) * fms - ms;
        }
        if (wait > 0) usleep((useconds_t)(wait * 1000) + 500);
    }
    fb_close();
    return 0;
}

/* metastreamer is taking the display: stop the splash and wait for it to let go of the framebuffer */
void splash_stop(void) {
    FILE *f = fopen(SPLASH_PID, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    unlink(SPLASH_PID);
    if (pid <= 1 || pid == getpid() || kill(pid, SIGTERM) < 0) return;
    for (int i = 0; i < 50 && kill(pid, 0) == 0; i++) usleep(10000);
}
