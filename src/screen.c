/*
 * screen.c — the status screen: what the bridge is waiting for, as a row of steps (USB, goggles, air unit,
 * video), plus Wi-Fi and power. Drawn in software with the DJI OSD font and icons from assets/
 * (tools/gen-ui.py -> ui_assets.h), shown on HDMI under the video and served as /snapshot while there is no video.
 */
#include "screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdmi.h"
#include "jpeg.h"
#include "ui_assets.h"

#define WHITE 0xFFFFFF
#define GREY 0x9A9A9A
#define DARK 0x5A5A5A
#define DARKER 0x333333
#define GREEN 0x00EE8B
#define ORANGE 0xEE8815
#define RED 0xF5222D

static uint32_t px[SCREEN_W * SCREEN_H];
static ScreenState cur;
static int drawn;
/* animations: frames of icons over the black background, played by HDMI in its own thread (hdmi_set_animation)
 * and by the splash; the JPEG shows the first frame */
typedef struct { const UiIcon *seq[32]; int n, ms, x, y; uint32_t *frames; } Anim;   /* x < 0: not on the screen */
static Anim anims[SCREEN_ANIMS];
static uint8_t *jpg; static size_t jpg_n; static int jpg_dirty = 1;
/* per row, the columns drawn on [ex0, ex1): what to clear before the next drawing, and all a copy needs to scale
 * (the rest is black). The ARM11 has no divide instruction: no divisions per pixel below. */
static uint16_t ex0[SCREEN_H], ex1[SCREEN_H];

static void mark(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    for (int j = y; j < y + h && w > 0; j++) {
        if (ex0[j] >= ex1[j]) { ex0[j] = x; ex1[j] = x + w; continue; }
        if (x < ex0[j]) ex0[j] = x;
        if (x + w > ex1[j]) ex1[j] = x + w;
    }
}

static void blend(int x, int y, uint32_t rgb, unsigned a) {
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H || !a) return;
    uint32_t *d = &px[y * SCREEN_W + x];
    if (a >= 255) { *d = rgb; return; }
    uint32_t o = 0;
    for (int s = 0; s < 24; s += 8) {
        unsigned c = (rgb >> s & 255) * a + (*d >> s & 255) * (255 - a) + 128;
        o |= ((c + (c >> 8)) >> 8) << s;   /* c / 255, rounded */
    }
    *d = o;
}

static void rect(int x, int y, int w, int h, uint32_t rgb) {
    mark(x, y, w, h);
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) blend(i, j, rgb, 255);
}

/* run-length pixels (count, then `size` bytes) into a w x h box at x, y */
static void icon(const UiIcon *ic, int x, int y) {
    const unsigned char *r = ic->rle;
    mark(x, y, ic->w, ic->h);
    for (int i = 0, n = ic->w * ic->h, cx = 0, cy = 0; i < n; r += 5)
        for (int k = 0; k < r[0]; k++, i++) {
            blend(x + cx, y + cy, (uint32_t)r[1] << 16 | r[2] << 8 | r[3], r[4]);
            if (++cx == ic->w) { cx = 0; cy++; }
        }
}

static const UiGlyph *glyph(const UiFont *f, unsigned char c) {
    return &f->g[(c < UI_FIRST || c > UI_LAST ? '?' : c) - UI_FIRST];
}

static int kern(const UiFont *f, char a, char b) {
    for (int i = 0; i < f->nkern; i++) if (f->kern[i][0] == a && f->kern[i][1] == b) return f->kern[i][2];
    return 0;
}

static int text_w(const UiFont *f, const char *s) {
    int w = 0;
    for (; *s; s++) w += glyph(f, (unsigned char)*s)->adv + kern(f, s[0], s[1]);
    return w;
}

/* left end of the text at x, baseline at y; returns where the next text would start */
static int text(const UiFont *f, int x, int y, const char *s, uint32_t rgb) {
    for (; *s; s++) {
        const UiGlyph *g = glyph(f, (unsigned char)*s);
        const unsigned char *r = g->rle;
        mark(x + g->x, y + g->y, g->w, g->h);
        for (int i = 0, n = g->w * g->h, cx = 0, cy = 0; i < n; r += 2)
            for (int k = 0; k < r[0]; k++, i++) {
                blend(x + g->x + cx, y + g->y + cy, rgb, r[1]);
                if (++cx == g->w) { cx = 0; cy++; }
            }
        x += g->adv + kern(f, s[0], s[1]);
    }
    return x;
}

static void text_c(const UiFont *f, int cx, int y, const char *s, uint32_t rgb) { text(f, cx - text_w(f, s) / 2, y, s, rgb); }

static void draw(const ScreenState *s) {
    for (int y = 0; y < SCREEN_H; y++)   /* clear what the last drawing left */
        if (ex0[y] < ex1[y]) memset(&px[y * SCREEN_W + ex0[y]], 0, (ex1[y] - ex0[y]) * 4);
    memset(ex0, 0, sizeof(ex0)); memset(ex1, 0, sizeof(ex1));
    for (int a = 0; a < SCREEN_ANIMS; a++) anims[a].x = -1;
    static const char *const title[] = {"NO GOGGLES", "CONNECTING", "NO SIGNAL", "NO VIDEO"};
    static const char *const hint[] = {
        "TURN ON THE GOGGLES AND CONNECT THE PI TO THEIR USB PORT",
        "REGISTERING WITH THE GOGGLES",
        "TURN ON THE AIR UNIT",
        "WAITING FOR VIDEO FROM THE GOGGLES"};
    static const char *const step[] = {"SYSTEM", "USB", "GOGGLES", "AIR UNIT", "VIDEO"};
    int st = s->stage < STAGE_USB ? STAGE_USB : s->stage > STAGE_VIDEO ? STAGE_VIDEO : s->stage;

    /* grey until the goggles are registered */
    const UiIcon *g = s->reconnecting ? &UI_ICON_GLASSES_RED
                    : s->starting || st <= STAGE_GOGGLES ? &UI_ICON_GLASSES_GREY : &UI_ICON_GLASSES_WHITE;
    icon(g, (SCREEN_W - g->w) / 2, 380 - g->h);
    const char *t = s->starting ? "STARTING" : s->reconnecting ? "RECONNECTING" : title[st];
    const char *h = s->starting ? (s->boot_step[0] ? s->boot_step : "STARTING THE SYSTEM")
                  : s->reconnecting ? "THE GOGGLES STOPPED ANSWERING: RESTARTING USB" : hint[st];
    text_c(&UI_FONT_TITLE, SCREEN_W / 2, 500, t, WHITE);
    text_c(&UI_FONT_TEXT, SCREEN_W / 2, 570, h, GREY);

    /* steps: done, the one being waited for, still to come; lines between them */
    /* the line from the boot to the video: SYSTEM is the boot itself, its line fills as the init scripts finish */
    const int n = 5, y = 700, gap = 280, x0 = SCREEN_W / 2 - gap * (n - 1) / 2, len = gap - 68;
    int at = s->starting ? 0 : st + 1;   /* the step being waited for */
    for (int i = 0; i < n - 1; i++) {
        int x = x0 + i * gap + 34, done = i < at ? len : 0;
        if (s->starting && i == 0 && s->boot_total > 0)
            done = len * (s->boot_done > s->boot_total ? s->boot_total : s->boot_done) / s->boot_total;
        rect(x, y - 2, len, 4, DARKER);
        rect(x, y - 2, done, 4, GREEN);
    }
    for (int i = 0; i < n; i++) {
        int cx = x0 + i * gap;
        const UiIcon *ic = i < at ? &UI_ICON_STEP_DONE
                         : i > at ? &UI_ICON_STEP_PENDING
                         : s->reconnecting ? &UI_ICON_STEP_FAILED : anims[ANIM_LOADER].seq[0];
        icon(ic, cx - ic->w / 2, y - ic->h / 2);
        if (i == at && !s->reconnecting) { anims[ANIM_LOADER].x = cx - ic->w / 2; anims[ANIM_LOADER].y = y - ic->h / 2; }
        text_c(&UI_FONT_SMALL, cx, y + 70, step[i], i < at ? GREY : i > at ? DARK : WHITE);
    }

    if (s->undervoltage) {
        const UiIcon *w = s->undervoltage > 1 ? &UI_ICON_WARNING_RED : &UI_ICON_WARNING_ORANGE;
        const char *m = s->undervoltage > 1 ? "LOW VOLTAGE: THE POWER SUPPLY CANNOT KEEP UP"
                                            : "LOW VOLTAGE WAS DETECTED: CHECK THE POWER SUPPLY";
        int tw = text_w(&UI_FONT_SMALL, m), x = (SCREEN_W - tw - w->w - 16) / 2;
        icon(w, x, 880 - w->h / 2);
        text(&UI_FONT_SMALL, x + w->w + 16, 880 + 10, m, s->undervoltage > 1 ? RED : ORANGE);
    }

    /* bottom line: Wi-Fi and the address of the web page; the name on the right */
    static const UiIcon *const bars[4] = {&UI_ICON_WIFI0, &UI_ICON_WIFI1, &UI_ICON_WIFI2, &UI_ICON_WIFI3};
    const UiIcon *wi = s->wifi == WIFI_OFF ? &UI_ICON_WIFI_OFF : s->wifi == WIFI_ERROR ? &UI_ICON_WIFI_ERROR
                     : s->wifi == WIFI_SEARCHING ? anims[ANIM_WIFI].seq[0] : bars[s->wifi_level & 3];
    const int by = 1010;
    icon(wi, 60, by - 34);
    if (s->wifi == WIFI_SEARCHING) { anims[ANIM_WIFI].x = 60; anims[ANIM_WIFI].y = by - 34; }   /* bars going up and down */
    int tx = 60 + wi->w + 20;
    if (s->wifi == WIFI_OFF) text(&UI_FONT_SMALL, tx, by, "WI-FI OFF", GREY);
    else if (s->wifi == WIFI_SEARCHING) text(&UI_FONT_SMALL, tx, by, "WI-FI: CONNECTING", GREY);
    else if (s->wifi == WIFI_ERROR) {
        char m[sizeof(s->wifi_error) + 8];
        snprintf(m, sizeof(m), "WI-FI: %s", s->wifi_error);
        for (char *c = m; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';
        text(&UI_FONT_SMALL, tx, by, m, RED);
    }
    else {
        char ssid[sizeof(s->ssid)], url[32];
        snprintf(ssid, sizeof(ssid), "%s", s->ssid);
        for (char *c = ssid; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';
        snprintf(url, sizeof(url), "http://%s", s->ip);
        tx = text(&UI_FONT_SMALL, tx, by, ssid, WHITE);
        text(&UI_FONT_SMALL, tx + 32, by, url, GREY);
    }
    text(&UI_FONT_SMALL, SCREEN_W - 60 - text_w(&UI_FONT_SMALL, "METASTREAMER"), by, "METASTREAMER", DARK);
}

/* the sequences, and their frames over the black background (before the first drawing: px is still black) */
static void anims_init(void) {
    Anim *l = &anims[ANIM_LOADER], *w = &anims[ANIM_WIFI];
    l->n = UI_LOADER_FRAMES > 32 ? 32 : UI_LOADER_FRAMES; l->ms = 40;
    for (int k = 0; k < l->n; k++) l->seq[k] = UI_LOADER[k];
    w->ms = 250;   /* 0 1 2 3 2 1 ... */
    for (int k = 0; k < UI_WIFI_SEARCH_FRAMES; k++) w->seq[w->n++] = UI_WIFI_SEARCH[k];
    for (int k = UI_WIFI_SEARCH_FRAMES - 2; k > 0 && w->n < 32; k--) w->seq[w->n++] = UI_WIFI_SEARCH[k];
    for (int a = 0; a < SCREEN_ANIMS; a++) {
        Anim *an = &anims[a];
        an->x = -1;
        int fw = an->seq[0]->w, fh = an->seq[0]->h;
        if (!(an->frames = calloc((size_t)an->n * fw * fh, 4))) continue;
        for (int k = 0; k < an->n; k++) {
            for (int y = 0; y < fh; y++) memset(px + y * SCREEN_W, 0, fw * 4);
            icon(an->seq[k], 0, 0);
            for (int y = 0; y < fh; y++) memcpy(an->frames + ((size_t)k * fh + y) * fw, px + y * SCREEN_W, fw * 4);
        }
    }
}

void screen_update(const ScreenState *s) {
    if (drawn && !memcmp(s, &cur, sizeof(cur))) return;
    if (!drawn) anims_init();
    cur = *s; drawn = 1;
    draw(s);
    jpg_dirty = 1;
    for (int a = 0; a < SCREEN_ANIMS; a++) {
        Anim *an = &anims[a];
        hdmi_set_animation(a, an->frames, an->n, an->seq[0]->w, an->seq[0]->h, an->frames ? an->x : -1, an->y, an->ms);
    }
    hdmi_show_screen(px, SCREEN_W, SCREEN_H);
}

int screen_anim_info(int a, int *n, int *ms) {
    if (a < 0 || a >= SCREEN_ANIMS || anims[a].x < 0) return 0;
    *n = anims[a].n; *ms = anims[a].ms;
    return 1;
}

int screen_anim_frame(int a, int k, int *x, int *y, int *w, int *h) {
    if (a < 0 || a >= SCREEN_ANIMS || anims[a].x < 0) return 0;
    Anim *an = &anims[a];
    const UiIcon *ic = an->seq[k % an->n];
    for (int j = 0; j < ic->h; j++) memset(&px[(an->y + j) * SCREEN_W + an->x], 0, ic->w * 4);
    icon(ic, an->x, an->y);
    *x = an->x; *y = an->y; *w = ic->w; *h = ic->h;
    return 1;
}

const uint8_t *screen_jpeg(size_t *n) {
    if (!drawn) { ScreenState s = {0}; screen_update(&s); }
    if (jpg_dirty) {
        free(jpg);
        jpg_n = jpeg_encode_xrgb(px, SCREEN_W, SCREEN_H, SCREEN_W, 85, &jpg);
        jpg_dirty = 0;
    }
    *n = jpg_n;
    return jpg;
}

const uint32_t *screen_pixels(void) { return px; }

void screen_extents(const uint16_t **x0, const uint16_t **x1) { *x0 = ex0; *x1 = ex1; }
