/*
 * telem.c — parsing of telemetry from the DUML that the goggles push to the registered app on their own.
 * Formats follow the YStreamer sources (device/backend/src/input/duml/{camera,link,devices,aircraft_recorder}.rs):
 *   06:1E                 goggles battery, payload[4] = %
 *   09:08 from 0x0E       radio link quality, payload[0] & 0x7F
 *   00:81 / 00:82         identity of linked devices: payload[0..32] = model code (ZV900, ...)
 *   02:80 (camera state)  recording: payload[0]&0x80, duration [29..30], storage total/free/left [5]/[9]/[17]
 *   02:81 (camera params) shutter, ISO, EV, exposure, WB, resolution, fps, ...
 * Plus a monitor of all message types (src,dst,reply,set,id) for research (/duml).
 */
#include "telem.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define STALE 5.0
#define MAX_ENT 192
#define HEAD 48

static double mono(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct {
    uint8_t src, dst, resp, set, id;
    unsigned long long count, changes;
    uint16_t len;
    uint8_t head[HEAD];
    double seen;
} Ent;
static Ent ents[MAX_ENT];
static int nents;

static int batt = -1, lq = -1; static double batt_t, lq_t;
static struct { uint8_t addr; char code[24]; double t; } devs[16];
static int ndev;

/* air unit camera: from push 02:81 and from poll replies (get = set+1) */
static struct {
    double t_push, t_any;
    int res_w, fps, exp_auto, iso, wb_auto, wb_temp, ev10, sharp, nr, color, flicker; unsigned shutter;
    unsigned char have_fmt, have_exp, have_shutter, have_iso, have_wb, have_ev, have_sharp, have_nr, have_color, have_flicker;
} cam;
static struct { int valid; double t; int recording; unsigned secs; unsigned total_mb, free_mb, left_s; } rec;

static const struct { uint8_t w; const char *res, *ar; } RES[] = {
    {10, "1080p", "16:9"}, {98, "1080p", "4:3"}, {45, "2.7K", "16:9"},
    {95, "2.7K", "4:3"}, {16, "4K", "16:9"}, {103, "4K", "4:3"},
};
static const int ISO[] = {0, 25, 50, 100, 200, 400, 800, 1600, 3200, 6400, 12800, 25600};
static const int EV10[] = {-30, -27, -23, -20, -17, -13, -10, -7, -3, 0, 3, 7, 10, 13, 17, 20, 23, 27, 30};
static const struct { uint8_t w; const char *n; } COLOR[] = {{0x00, "normal"}, {0x06, "dcinelike"}, {0x3D, "dlog"}};
static const struct { uint8_t w; const char *n; } FLICKER[] = {{0, "auto"}, {1, "60hz"}, {2, "50hz"}, {3, "off"}};

static unsigned u32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

#define PUT(...) do { if (o < cap) o += snprintf(out + o, cap - o, __VA_ARGS__); } while (0)

/* raw ring: the last RAW_N frames in full, for the DUML console (/api/duml/send) */
#define RAW_N 64
#define RAW_PL 200
typedef struct { unsigned seq; uint8_t src, dst, resp, set, id, len; uint8_t pl[RAW_PL]; } Raw;
static Raw raws[RAW_N];
static unsigned raw_seq;

static void raw_record(uint8_t src, uint8_t dst, int resp, uint8_t set, uint8_t id, const uint8_t *p, size_t n) {
    unsigned sq = ++raw_seq;
    Raw *r = &raws[sq % RAW_N];
    r->seq = sq; r->src = src; r->dst = dst; r->resp = resp; r->set = set; r->id = id;
    r->len = n > RAW_PL ? RAW_PL : (uint8_t)n;
    memcpy(r->pl, p, r->len);
}

unsigned telem_raw_seq(void) { return raw_seq; }

size_t telem_raw_json(unsigned since, uint8_t src, char *out, size_t cap) {
    size_t o = 0;
    PUT("[");
    int first = 1;
    for (unsigned s = since + 1; s <= raw_seq; s++) {
        if (raw_seq - s >= RAW_N) continue;   /* overwritten */
        Raw *r = &raws[s % RAW_N];
        if (r->seq != s || (src && r->src != src)) continue;
        PUT("%s{\"src\":\"0x%02x\",\"dst\":\"0x%02x\",\"resp\":%s,\"cmd\":\"%02x:%02x\",\"payload\":\"",
            first ? "" : ",", r->src, r->dst, r->resp ? "true" : "false", r->set, r->id);
        for (int i = 0; i < r->len; i++) PUT("%02x", r->pl[i]);
        PUT("\"}");
        first = 0;
    }
    PUT("]");
    return o < cap ? o : cap - 1;
}

static void record(uint8_t src, uint8_t dst, int resp, uint8_t set, uint8_t id, const uint8_t *p, size_t n) {
    Ent *e = NULL;
    for (int i = 0; i < nents; i++)
        if (ents[i].src == src && ents[i].dst == dst && ents[i].resp == resp && ents[i].set == set && ents[i].id == id) { e = &ents[i]; break; }
    if (!e) {
        if (nents >= MAX_ENT) return;
        e = &ents[nents++];
        memset(e, 0, sizeof(*e));
        e->src = src; e->dst = dst; e->resp = resp; e->set = set; e->id = id;
    }
    size_t h = n < HEAD ? n : HEAD;
    if (e->count == 0 || e->len != n || memcmp(e->head, p, h)) e->changes++;
    e->count++; e->len = n; memcpy(e->head, p, h); e->seen = mono();
}

void telem_on_duml(uint8_t src, uint8_t dst, uint8_t flags, uint8_t set, uint8_t id, const uint8_t *p, size_t n) {
    int resp = (flags & 0x80) != 0;
    record(src, dst, resp, set, id, p, n);
    raw_record(src, dst, resp, set, id, p, n);
    double t = mono();

    if (set == 6 && id == 30 && n > 4) { batt = p[4]; batt_t = t; }
    else if (set == 9 && id == 8 && src == 0x0E && !resp && n >= 1) { lq = p[0] & 0x7F; lq_t = t; }
    else if (set == 0 && (id == 0x81 || id == 0x82) && n >= 32) {
        char code[24]; size_t k = 0;
        while (k < sizeof(code) - 1 && k < 32 && p[k] > 0x20 && p[k] < 0x7F) { code[k] = p[k]; k++; }
        code[k] = 0;
        if (k) {
            int i; for (i = 0; i < ndev && devs[i].addr != src; i++) {}
            if (i == ndev && ndev < 16) ndev++;
            if (i < 16) { devs[i].addr = src; memcpy(devs[i].code, code, k + 1); devs[i].t = t; }
        }
    } else if (set == 2 && id == 128 && n >= 31) {
        rec.recording = (p[0] & 0x80) != 0;
        rec.secs = p[29] | (p[30] << 8);
        rec.total_mb = u32le(p + 5); rec.free_mb = u32le(p + 9); rec.left_s = u32le(p + 17);
        rec.valid = 1; rec.t = t;
    } else if (set == 2 && id == 129 && n >= 25 && !resp) {
        unsigned sh = p[2] | (p[3] << 8);
        cam.shutter = sh >= 32768 ? sh - 32768 : 0; cam.have_shutter = 1;
        if (p[5] == 0 || p[5] == 0xFF) { cam.iso = 0; cam.have_iso = 1; }
        else if (p[5] < 12) { cam.iso = ISO[p[5]]; cam.have_iso = 1; }
        if (p[6] >= 7 && p[6] - 7 < 19) { cam.ev10 = EV10[p[6] - 7]; cam.have_ev = 1; }
        if (p[20] == 0x01 || p[20] == 0x04) { cam.exp_auto = p[20] == 0x01; cam.have_exp = 1; }
        if (p[23] == 0x00 || p[23] == 0x06) { cam.wb_auto = p[23] == 0x00; if (p[24]) cam.wb_temp = p[24] * 100; cam.have_wb = 1; }
        cam.res_w = p[13]; cam.fps = p[14]; cam.have_fmt = 1;
        if (n > 27) { cam.sharp = (int8_t)p[27]; cam.have_sharp = 1; }
        if (n > 32) { cam.color = p[31]; cam.flicker = p[32]; cam.have_color = cam.have_flicker = 1; }
        if (n > 103) { cam.nr = (int8_t)p[103]; cam.have_nr = 1; }
        cam.t_push = cam.t_any = t;
    } else if (set == 2 && resp && n >= 2 && p[0] == 0) {
        const uint8_t *v = p + 1; size_t vn = n - 1;
        switch (id - 1) {
        case 24: if (vn >= 2) { cam.res_w = v[0]; cam.fps = v[1]; cam.have_fmt = 1; } break;
        case 30: if (v[0] == 1 || v[0] == 4) { cam.exp_auto = v[0] == 1; cam.have_exp = 1; } break;
        case 40: if (vn >= 3) { unsigned d = v[1] | (v[2] << 8); cam.shutter = d >= 32768 ? d - 32768 : 0; cam.have_shutter = 1; } break;
        case 42: if (v[0] == 0 || v[0] == 0xFF) { cam.iso = 0; cam.have_iso = 1; } else if (v[0] < 12) { cam.iso = ISO[v[0]]; cam.have_iso = 1; } break;
        case 44: if (v[0] == 0) { cam.wb_auto = 1; cam.have_wb = 1; }
                 else if (v[0] == 6 && vn >= 3) { cam.wb_auto = 0; cam.wb_temp = (v[1] | (v[2] << 8)) * 100; cam.have_wb = 1; } break;
        case 46: if (v[0] >= 7 && v[0] - 7 < 19) { cam.ev10 = EV10[v[0] - 7]; cam.have_ev = 1; } break;
        case 56: cam.sharp = (int8_t)v[0]; cam.have_sharp = 1; break;
        case 66: cam.color = v[0]; cam.have_color = 1; break;
        case 68: cam.nr = (int8_t)v[0]; cam.have_nr = 1; break;
        case 70: cam.flicker = v[0]; cam.have_flicker = 1; break;
        default: return;
        }
        cam.t_any = t;
    }
}

/* whether an air unit is linked (O3 WM1695, O4 ZA530x, V1 WM150/LT150, ...) */
static int have_aircraft(void) {
    double t = mono();
    for (int i = 0; i < ndev; i++)
        if (t - devs[i].t < STALE * 2 && (!strncmp(devs[i].code, "WM", 2) || !strncmp(devs[i].code, "ZA", 2) ||
                                          !strncmp(devs[i].code, "LT", 2) || !strncmp(devs[i].code, "WA", 2))) return 1;
    return 0;
}

int telem_camera_needs_poll(void) { return have_aircraft() && mono() - cam.t_push > 3.0; }

int telem_battery(void) { return (batt >= 0 && mono() - batt_t < STALE) ? batt : -1; }
int telem_link_quality(void) { return (lq >= 0 && mono() - lq_t < STALE) ? lq : -1; }
int telem_recording(void) { return (rec.valid && mono() - rec.t < STALE) ? rec.recording : -1; }


size_t telem_json(char *out, size_t cap) {
    size_t o = 0; double t = mono();
    PUT("{\"goggles_battery\":");
    if (telem_battery() >= 0) PUT("%d", batt); else PUT("null");
    PUT(",\"link_quality\":");
    if (telem_link_quality() >= 0) PUT("%d", lq); else PUT("null");
    PUT(",\"devices\":[");
    for (int i = 0, first = 1; i < ndev; i++) {
        if (t - devs[i].t > STALE * 2) continue;
        PUT("%s{\"addr\":%u,\"code\":\"%s\"}", first ? "" : ",", devs[i].addr, devs[i].code); first = 0;
    }
    PUT("],\"camera\":");
    if (cam.t_any > 0 && t - cam.t_any < STALE * 6) {
        PUT("{\"age_s\":%.1f", t - cam.t_any);
        if (cam.have_fmt) {
            const char *r = "?", *a = "?";
            for (size_t i = 0; i < sizeof(RES) / sizeof(RES[0]); i++) if (RES[i].w == cam.res_w) { r = RES[i].res; a = RES[i].ar; }
            int hz = telem_fps_hz(cam.fps);
            if (hz > 0) PUT(",\"res\":\"%s\",\"ar\":\"%s\",\"fps\":%d", r, a, hz);
            else PUT(",\"res\":\"%s\",\"ar\":\"%s\",\"fps\":null,\"fps_wire\":%d", r, a, cam.fps);
        }
        if (cam.have_exp) PUT(",\"exposure\":\"%s\"", cam.exp_auto ? "auto" : "manual");
        if (cam.have_shutter) PUT(",\"shutter\":%u", cam.shutter);
        if (cam.have_iso) PUT(",\"iso\":%d", cam.iso);
        if (cam.have_ev) PUT(",\"ev\":%.1f", cam.ev10 / 10.0);
        if (cam.have_wb) PUT(",\"wb\":\"%s\",\"wb_temp\":%d", cam.wb_auto ? "auto" : "manual", cam.wb_temp);
        if (cam.have_sharp) PUT(",\"sharpness\":%d", cam.sharp);
        if (cam.have_nr) PUT(",\"noise_reduction\":%d", cam.nr);
        if (cam.have_color) { const char *c = "?"; for (size_t i = 0; i < 3; i++) if (COLOR[i].w == cam.color) c = COLOR[i].n; PUT(",\"color\":\"%s\"", c); }
        if (cam.have_flicker) { const char *f = "?"; for (size_t i = 0; i < 4; i++) if (FLICKER[i].w == cam.flicker) f = FLICKER[i].n; PUT(",\"flicker\":\"%s\"", f); }
        PUT("}");
    } else PUT("null");
    PUT(",\"air_recorder\":");
    if (rec.valid && t - rec.t < STALE)
        PUT("{\"recording\":%s,\"record_secs\":%u,\"total_mb\":%u,\"free_mb\":%u,\"secs_left\":%u}",
            rec.recording ? "true" : "false", rec.secs, rec.total_mb, rec.free_mb, rec.left_s);
    else PUT("null");
    PUT("}");
    return o < cap ? o : cap - 1;
}

size_t telem_duml_json(char *out, size_t cap) {
    size_t o = 0; double t = mono();
    PUT("[");
    for (int i = 0; i < nents; i++) {
        Ent *e = &ents[i];
        PUT("%s{\"src\":\"0x%02x\",\"dst\":\"0x%02x\",\"resp\":%s,\"cmd\":\"%02x:%02x\",\"count\":%llu,\"changes\":%llu,"
            "\"len\":%u,\"age_ms\":%d,\"head\":\"",
            i ? "," : "", e->src, e->dst, e->resp ? "true" : "false", e->set, e->id, e->count, e->changes,
            e->len, (int)((t - e->seen) * 1000));
        int h = e->len < HEAD ? e->len : HEAD;
        for (int k = 0; k < h; k++) PUT("%02x", e->head[k]);
        PUT("\"}");
        if (o + 256 > cap) break;
    }
    PUT("]");
    return o < cap ? o : cap - 1;
}

/* camera frame-rate wire codes (from YStreamer cameraModels.ts) */
static const struct { int hz, wire; } FPS_TAB[] = {{30, 3}, {48, 15}, {50, 5}, {60, 6}, {100, 10}, {120, 7}};

int telem_fps_hz(int wire) {
    for (size_t i = 0; i < sizeof(FPS_TAB) / sizeof(FPS_TAB[0]); i++) if (FPS_TAB[i].wire == wire) return FPS_TAB[i].hz;
    return -1;
}

int telem_fps_wire(int hz) {
    for (size_t i = 0; i < sizeof(FPS_TAB) / sizeof(FPS_TAB[0]); i++) if (FPS_TAB[i].hz == hz) return FPS_TAB[i].wire;
    return -1;
}
