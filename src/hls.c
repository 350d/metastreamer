/*
 * hls.c — raw H.264 (Annex-B) -> fMP4 + built-in HTTP server. No transcoding.
 *
 *  /live.mp4    endless fMP4: init, then each frame as a separate moof+mdat fragment.
 *               The page feeds it into MediaSource (Safari macOS; iOS 17.1+ via ManagedMediaSource).
 *               Latency ~0.2-0.4 s.
 *  /stream.m3u8 HLS with per-GOP fMP4 segments (fallback, latency 2-3 s).
 *  /info.json   codec and dimensions for the page.
 *  /            player page.
 *  /settings   admin page; /api/set (camera), /api/wifi (Wi-Fi networks), /api/time (time zone), /api/time/sync (POST), /api/net (address).
 * Completed frames are also passed to a sink (HDMI) via hls_set_au_sink().
 */
#define _GNU_SOURCE   /* strcasestr */
#include "hls.h"
#include "hdmi.h"
#include "telem.h"
#include "wifi.h"
#include "conf.h"
#include "settings_html.h"

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TIMESCALE 90000
#define SEG_TARGET 0.4          /* s: cut HLS at every keyframe (GOP ~0.6 s) */
#define RING 10                 /* HLS segments kept in memory */
#define LIST 6                  /* HLS segments in the playlist */
#define MAX_SAMPLES 900
#define GAP_RESET 1.5           /* s of silence -> stream reset */
#define MAX_CONN 12
#define LIVE_BACKLOG (4 * 1024 * 1024) /* a lagging live client is disconnected */
#define SSE_PERIOD 0.25             /* s: /events period */
#define SNAP_TIMEOUT 2.0

/* ---------------- byte buffer and box writing ---------------- */
typedef struct { uint8_t *p; size_t n, cap; } B;

static void bgrow(B *b, size_t need) {
    if (b->n + need <= b->cap) return;
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + need) c *= 2;
    uint8_t *np = realloc(b->p, c);
    if (!np) { perror("realloc"); exit(1); }
    b->p = np; b->cap = c;
}
static void bput(B *b, const void *d, size_t n) { bgrow(b, n); memcpy(b->p + b->n, d, n); b->n += n; }
static void b8(B *b, unsigned v) { uint8_t x = v; bput(b, &x, 1); }
static void b16(B *b, unsigned v) { b8(b, v >> 8); b8(b, v); }
static void b32(B *b, uint32_t v) { b16(b, v >> 16); b16(b, v); }
static void b64(B *b, uint64_t v) { b32(b, v >> 32); b32(b, (uint32_t)v); }
static void bzero_(B *b, size_t n) { bgrow(b, n); memset(b->p + b->n, 0, n); b->n += n; }
static size_t bopen(B *b, const char *t) { size_t at = b->n; b32(b, 0); bput(b, t, 4); return at; }
static void bclose(B *b, size_t at) {
    uint32_t s = b->n - at;
    b->p[at] = s >> 24; b->p[at + 1] = s >> 16; b->p[at + 2] = s >> 8; b->p[at + 3] = s;
}
static void bfull(B *b, unsigned ver, unsigned flags) { b32(b, (ver << 24) | (flags & 0xFFFFFF)); }

static double mono(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------- SPS parsing (width/height) ---------------- */
typedef struct { const uint8_t *p; size_t n, bit; } BR;
static unsigned rb(BR *r) {
    if (r->bit >= r->n * 8) return 0;
    unsigned v = (r->p[r->bit >> 3] >> (7 - (r->bit & 7))) & 1; r->bit++; return v;
}
static unsigned rbits(BR *r, int k) { unsigned v = 0; while (k--) v = (v << 1) | rb(r); return v; }
static unsigned ue(BR *r) { int z = 0; while (!rb(r) && z < 32) z++; return ((1u << z) - 1) + rbits(r, z); }
static int se(BR *r) { unsigned k = ue(r); return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2); }

static void sps_dims(const uint8_t *sps, size_t n, int *w, int *h, int *chroma) {
    uint8_t rbsp[256]; size_t m = 0;
    for (size_t i = 1; i < n && m < sizeof(rbsp); i++) {
        if (i >= 3 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0) continue;
        rbsp[m++] = sps[i];
    }
    BR r = {rbsp, m, 0};
    unsigned profile = rbits(&r, 8); rbits(&r, 8); rbits(&r, 8); ue(&r);
    unsigned cf = 1, sep = 0;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        cf = ue(&r); if (cf == 3) sep = rb(&r);
        ue(&r); ue(&r); rb(&r);
        if (rb(&r)) {
            for (int i = 0; i < (cf != 3 ? 8 : 12); i++) if (rb(&r)) {
                int sz = i < 6 ? 16 : 64, last = 8, next = 8;
                for (int j = 0; j < sz; j++) { if (next) next = (last + se(&r) + 256) % 256; last = next ? next : last; }
            }
        }
    }
    ue(&r);
    unsigned poc = ue(&r);
    if (poc == 0) ue(&r);
    else if (poc == 1) { rb(&r); se(&r); se(&r); unsigned c = ue(&r); for (unsigned i = 0; i < c && i < 256; i++) se(&r); }
    ue(&r); rb(&r);
    unsigned wm = ue(&r), hm = ue(&r), fmo = rb(&r);
    if (!fmo) rb(&r);
    rb(&r);
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (rb(&r)) { cl = ue(&r); cr = ue(&r); ct = ue(&r); cb = ue(&r); }
    int sw = (cf == 1 || cf == 2) ? 2 : 1, sh = (cf == 1) ? 2 : 1;
    int ux = (cf == 0 || sep) ? 1 : sw, uy = ((cf == 0 || sep) ? 1 : sh) * (2 - fmo);
    *w = (wm + 1) * 16 - (cl + cr) * ux;
    *h = (2 - fmo) * (hm + 1) * 16 - (ct + cb) * uy;
    *chroma = cf;
    if (*w <= 0 || *w > 8192 || *h <= 0 || *h > 8192) { *w = 1920; *h = 1080; }
}

/* ---------------- stream state ---------------- */
static uint8_t sps[256], pps[256]; static size_t sps_n, pps_n;
static int have_init; static B init_seg; static int vw, vh;
static au_sink_fn au_sink;
static B annexb;

typedef struct { B data; uint32_t seq; double dur; double rt; char meta[128]; } Seg;
static Seg ring[RING]; static int ring_n; static uint32_t next_seq = 1;

/* HLS segment under construction */
static B sdata; static uint32_t s_size[MAX_SAMPLES], s_dur[MAX_SAMPLES]; static uint8_t s_key[MAX_SAMPLES];
static int s_cnt; static uint64_t s_base; static int open_seg;

/* timing */
static uint64_t last_ts; static int ts_started;
static double last_feed;

/* Frame durations. Arrival times carry the radio's jitter, so every frame gets the stream's nominal interval
 * (its long-run rate snapped to a standard rate), nudged by at most 5 % so that the timeline follows the
 * wall clock on average: the player's buffer then neither drains nor grows. After a pause or a burst of
 * more than LATE the timeline is re-anchored instead. Used by the live stream and the HLS segments. */
static double nom_dt = 0.02, rate_t0; static unsigned long long rate_n0;
static double tl_t0, live_t0;   /* wall time of position 0 on each timeline */
#define LATE 0.3
static uint32_t frame_dur(double t, double *t0, uint64_t pos) {
    double behind = (t - *t0) - (double)pos / TIMESCALE;   /* > 0: the timeline lags the wall clock */
    if (behind > LATE || behind < -LATE) { *t0 = t - (double)pos / TIMESCALE; behind = 0; }
    double dd = nom_dt * (1.0 + fmax(-0.05, fmin(0.05, behind / 2.0)));
    return (uint32_t)(dd * TIMESCALE + 0.5);
}
/* live stream: each frame is sent immediately, continuous timeline */
static uint64_t live_dts; static double last_au_t;
/* arrival statistics: intervals of the last ARR_N frames, long-run rate since the first frame */
#define ARR_N 256
static float arr_ms[ARR_N]; static unsigned arr_i; static double long_t0; static unsigned long long long_n0;
static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static uint32_t live_seq = 1;

static unsigned long long n_frames, n_segs, n_resets, n_live_frags, n_bytes;
static double win_t, rate_fps, rate_kbps; static unsigned long long win_frames, win_bytes;

/* current frame (access unit) in AVCC format */
static B au; static int au_vcl, au_key;
/* Annex-B input buffer */
static B es;

void hls_set_au_sink(au_sink_fn f) { au_sink = f; }

static json_fn link_fn;
static double t_start;
void hls_set_link_fn(json_fn f) { link_fn = f; }
static set_fn cam_fn;
void hls_set_camera_fn(set_fn f) { cam_fn = f; }
static duml_fn duml_send_fn;
void hls_set_duml_fn(duml_fn f) { duml_send_fn = f; }
#define DUML_WAIT 0.4   /* s to collect replies for /api/duml/send */

/* %XX and '+' in the query string */
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++, o++) {
        if (*s == '+') *o = ' ';
        else if (*s == '%' && s[1] && s[2]) { unsigned v; sscanf(s + 1, "%2x", &v); *o = (char)v; s += 2; }
        else *o = *s;
    }
    *o = 0;
}

static int live_count(void);
static size_t status_json(char *out, size_t cap) {
    size_t o = 0;
    char lk[256] = "null", hd[512] = "null";
    static char tm[4096];
    float sorted[ARR_N], aq[4] = {0, 0, 0, 0};
    unsigned na = arr_i < ARR_N ? arr_i : ARR_N;
    if (na) {
        memcpy(sorted, arr_ms, na * sizeof(float)); qsort(sorted, na, sizeof(float), cmp_f);
        aq[0] = sorted[na / 2]; aq[1] = sorted[na * 95 / 100]; aq[2] = sorted[na * 99 / 100]; aq[3] = sorted[na - 1];
    }
    if (link_fn) link_fn(lk, sizeof(lk));
    hdmi_json(hd, sizeof(hd));
    telem_json(tm, sizeof(tm));
    o += snprintf(out + o, cap - o,
        "{\"uptime_s\":%.1f,\"usb\":%s,\"video\":{\"width\":%d,\"height\":%d,\"codec\":\"avc1.%02X%02X%02X\","
        "\"fps\":%.1f,\"fps_long\":%.3f,\"nominal_fps\":%.2f,\"arrival_ms\":{\"p50\":%.1f,\"p95\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
        "\"kbps\":%.0f,\"frames\":%llu,\"live_clients\":%d,\"hls_segments\":%d},"
        "\"telemetry\":%s,\"hdmi\":%s}",
        mono() - t_start, lk, vw, vh, sps[1], sps[2], sps[3], rate_fps,
        long_t0 > 0 && last_au_t > long_t0 ? (n_frames - long_n0) / (last_au_t - long_t0) : 0.0, 1.0 / nom_dt,
        aq[0], aq[1], aq[2], aq[3], rate_kbps, n_frames, live_count(), ring_n, tm, hd);
    return o < cap ? o : cap - 1;
}

static void build_init(void) {
    int chroma = 1;
    sps_dims(sps, sps_n, &vw, &vh, &chroma);
    B *b = &init_seg; b->n = 0;
    size_t a = bopen(b, "ftyp"); bput(b, "iso5", 4); b32(b, 512); bput(b, "iso5iso6mp41", 12); bclose(b, a);
    size_t moov = bopen(b, "moov");
    size_t x = bopen(b, "mvhd"); bfull(b, 0, 0); b32(b, 0); b32(b, 0); b32(b, 1000); b32(b, 0);
    b32(b, 0x00010000); b16(b, 0x0100); bzero_(b, 10);
    b32(b, 0x00010000); b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0x00010000); b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0x40000000);
    bzero_(b, 24); b32(b, 2); bclose(b, x);
    size_t trak = bopen(b, "trak");
    x = bopen(b, "tkhd"); bfull(b, 0, 3); b32(b, 0); b32(b, 0); b32(b, 1); b32(b, 0); b32(b, 0); bzero_(b, 8);
    b16(b, 0); b16(b, 0); b16(b, 0); b16(b, 0);
    b32(b, 0x00010000); b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0x00010000); b32(b, 0); b32(b, 0); b32(b, 0); b32(b, 0x40000000);
    b32(b, (uint32_t)vw << 16); b32(b, (uint32_t)vh << 16); bclose(b, x);
    size_t mdia = bopen(b, "mdia");
    x = bopen(b, "mdhd"); bfull(b, 0, 0); b32(b, 0); b32(b, 0); b32(b, TIMESCALE); b32(b, 0); b16(b, 0x55C4); b16(b, 0); bclose(b, x);
    x = bopen(b, "hdlr"); bfull(b, 0, 0); b32(b, 0); bput(b, "vide", 4); bzero_(b, 12); bput(b, "VideoHandler", 13); bclose(b, x);
    size_t minf = bopen(b, "minf");
    x = bopen(b, "vmhd"); bfull(b, 0, 1); b16(b, 0); b16(b, 0); b16(b, 0); b16(b, 0); bclose(b, x);
    size_t dinf = bopen(b, "dinf"); size_t dref = bopen(b, "dref"); bfull(b, 0, 0); b32(b, 1);
    x = bopen(b, "url "); bfull(b, 0, 1); bclose(b, x); bclose(b, dref); bclose(b, dinf);
    size_t stbl = bopen(b, "stbl");
    size_t stsd = bopen(b, "stsd"); bfull(b, 0, 0); b32(b, 1);
    size_t avc1 = bopen(b, "avc1"); bzero_(b, 6); b16(b, 1); b16(b, 0); b16(b, 0); bzero_(b, 12);
    b16(b, vw); b16(b, vh); b32(b, 0x00480000); b32(b, 0x00480000); b32(b, 0); b16(b, 1);
    bzero_(b, 32); b16(b, 0x0018); b16(b, 0xFFFF);
    size_t avcc = bopen(b, "avcC");
    b8(b, 1); b8(b, sps[1]); b8(b, sps[2]); b8(b, sps[3]); b8(b, 0xFF);
    b8(b, 0xE1); b16(b, sps_n); bput(b, sps, sps_n);
    b8(b, 1); b16(b, pps_n); bput(b, pps, pps_n);
    unsigned prof = sps[1];
    if (prof == 100 || prof == 110 || prof == 122 || prof == 144) {
        b8(b, 0xFC | (chroma & 3)); b8(b, 0xF8); b8(b, 0xF8); b8(b, 0);
    }
    bclose(b, avcc); bclose(b, avc1); bclose(b, stsd);
    x = bopen(b, "stts"); bfull(b, 0, 0); b32(b, 0); bclose(b, x);
    x = bopen(b, "stsc"); bfull(b, 0, 0); b32(b, 0); bclose(b, x);
    x = bopen(b, "stsz"); bfull(b, 0, 0); b32(b, 0); b32(b, 0); bclose(b, x);
    x = bopen(b, "stco"); bfull(b, 0, 0); b32(b, 0); bclose(b, x);
    bclose(b, stbl); bclose(b, minf); bclose(b, mdia); bclose(b, trak);
    size_t mvex = bopen(b, "mvex");
    x = bopen(b, "trex"); bfull(b, 0, 0); b32(b, 1); b32(b, 1); b32(b, 0); b32(b, 0); b32(b, 0); bclose(b, x);
    bclose(b, mvex); bclose(b, moov);
    have_init = 1;
    printf("[hls] init %dx%d avc1.%02X%02X%02X\n", vw, vh, sps[1], sps[2], sps[3]);
}

/* moof+mdat from cnt frames (used both for HLS segments and for per-frame live fragments) */
static void build_frag(B *b, uint32_t seq, uint64_t base, int cnt, const uint32_t *size, const uint32_t *dur,
                       const uint8_t *key, const uint8_t *data, size_t dlen) {
    size_t moof = bopen(b, "moof");
    size_t x = bopen(b, "mfhd"); bfull(b, 0, 0); b32(b, seq); bclose(b, x);
    size_t traf = bopen(b, "traf");
    x = bopen(b, "tfhd"); bfull(b, 0, 0x020000); b32(b, 1); bclose(b, x);
    x = bopen(b, "tfdt"); bfull(b, 1, 0); b64(b, base); bclose(b, x);
    size_t trun = bopen(b, "trun"); bfull(b, 0, 0x000701); b32(b, cnt);
    size_t off_at = b->n; b32(b, 0);
    for (int i = 0; i < cnt; i++) { b32(b, dur[i]); b32(b, size[i]); b32(b, key[i] ? 0x02000000 : 0x01010000); }
    bclose(b, trun); bclose(b, traf); bclose(b, moof);
    uint32_t off = (uint32_t)(b->n - moof) + 8;
    b->p[off_at] = off >> 24; b->p[off_at + 1] = off >> 16; b->p[off_at + 2] = off >> 8; b->p[off_at + 3] = off;
    b32(b, (uint32_t)(dlen + 8)); bput(b, "mdat", 4); bput(b, data, dlen);
}

/* ---------------- HTTP connections ---------------- */
typedef struct { int fd; char in[8192]; size_t inlen; B out; size_t outpos; double t; int live, wait_key, closing, sse, snap_wait; unsigned snap_seq0; double snap_t0;
                 int duml_wait; unsigned duml_since; uint8_t duml_src; double duml_t0, duml_for; } Conn;
static Conn conns[MAX_CONN];
static int lsock = -1;
static unsigned long long n_req;

static int live_count(void) { int n = 0; for (int i = 0; i < MAX_CONN; i++) n += conns[i].fd >= 0 && conns[i].live; return n; }

static void conn_close(Conn *c) {
    close(c->fd); c->fd = -1; c->out.n = 0; c->outpos = 0; c->inlen = 0; c->live = c->closing = c->sse = c->snap_wait = c->duml_wait = 0;
}

static void close_live_all(void) {
    for (int i = 0; i < MAX_CONN; i++) if (conns[i].fd >= 0 && conns[i].live) conn_close(&conns[i]);
}

/* one frame to all live-stream subscribers */
static void emit_live(const B *a, int key, uint64_t base, uint32_t dur) {
    static B frag;
    int any = 0;
    for (int i = 0; i < MAX_CONN; i++) if (conns[i].fd >= 0 && conns[i].live && !(conns[i].wait_key && !key)) { any = 1; break; }
    if (!any) return;
    frag.n = 0;
    uint32_t size = a->n; uint8_t k = key;
    build_frag(&frag, live_seq++, base, 1, &size, &dur, &k, a->p, a->n);
    n_live_frags++;
    for (int i = 0; i < MAX_CONN; i++) {
        Conn *c = &conns[i];
        if (c->fd < 0 || !c->live) continue;
        if (c->wait_key) { if (!key) continue; c->wait_key = 0; }
        if (c->out.n - c->outpos > LIVE_BACKLOG) { conn_close(c); continue; }
        if (c->outpos > 0 && c->outpos == c->out.n) { c->out.n = 0; c->outpos = 0; }
        else if (c->outpos > 256 * 1024) {   /* drop already-sent data */
            memmove(c->out.p, c->out.p + c->outpos, c->out.n - c->outpos); c->out.n -= c->outpos; c->outpos = 0;
        }
        bput(&c->out, frag.p, frag.n);
        /* send right away, without waiting for the next poll pass */
        ssize_t w = send(c->fd, c->out.p + c->outpos, c->out.n - c->outpos, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) { c->outpos += w; if (c->outpos == c->out.n) { c->out.n = 0; c->outpos = 0; } }
        else if (w < 0 && errno != EAGAIN && errno != EINTR) conn_close(c);
    }
}

static void reset_stream(const char *why) {
    open_seg = 0; s_cnt = 0; sdata.n = 0; ring_n = 0; ts_started = 0; last_au_t = 0; n_resets++;
    close_live_all();   /* the page reconnects by itself */
    printf("[hls] reset: %s\n", why);
}

static void close_seg(void) {
    if (!open_seg || s_cnt == 0) { open_seg = 0; return; }
    Seg *sg;
    if (ring_n < RING) sg = &ring[ring_n++];
    else { B old = ring[0].data; memmove(ring, ring + 1, sizeof(Seg) * (RING - 1)); sg = &ring[RING - 1]; sg->data = old; }
    sg->data.n = 0;
    uint32_t seq = next_seq++;
    build_frag(&sg->data, seq, s_base, s_cnt, s_size, s_dur, s_key, sdata.p, sdata.n);
    uint64_t total = 0; for (int i = 0; i < s_cnt; i++) total += s_dur[i];
    sg->seq = seq; sg->dur = (double)total / TIMESCALE;
    { struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt); sg->rt = rt.tv_sec + rt.tv_nsec / 1e9 - sg->dur; }
    { int lq = telem_link_quality(), bt = telem_battery(), rc = telem_recording(); size_t m = 0; sg->meta[0] = 0;
      if (lq >= 0) m += snprintf(sg->meta + m, sizeof(sg->meta) - m, ",X-LINK-QUALITY=%d", lq);
      if (bt >= 0) m += snprintf(sg->meta + m, sizeof(sg->meta) - m, ",X-GOGGLES-BATTERY=%d", bt);
      if (rc >= 0) m += snprintf(sg->meta + m, sizeof(sg->meta) - m, ",X-AIR-RECORDING=%d", rc); }
    n_segs++;
    open_seg = 0; s_cnt = 0; sdata.n = 0;
}

/* pass the frame to the sink (HDMI) as Annex-B */
static void sink_au(void) {
    if (!au_sink) return;
    annexb.n = 0;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    if (au_key) { bput(&annexb, sc, 4); bput(&annexb, sps, sps_n); bput(&annexb, sc, 4); bput(&annexb, pps, pps_n); }
    for (size_t o = 0; o + 4 <= au.n;) {
        uint32_t l = (au.p[o] << 24) | (au.p[o + 1] << 16) | (au.p[o + 2] << 8) | au.p[o + 3];
        o += 4; if (o + l > au.n) break;
        bput(&annexb, sc, 4); bput(&annexb, au.p + o, l); o += l;
    }
    au_sink(annexb.p, annexb.n, au_key);
}

/* completed frame */
static void push_au(void) {
    if (!au_vcl) { au.n = 0; au_key = 0; return; }
    double t = mono();
    if (last_feed > 0 && t - last_feed > GAP_RESET && ts_started) reset_stream("stream pause");
    last_feed = t;
    if (!have_init) { au.n = 0; au_vcl = au_key = 0; return; }
    sink_au();

    /* nominal interval from the last 4 s, snapped to a standard rate */
    if (rate_t0 == 0) { rate_t0 = t; rate_n0 = n_frames; }
    else if (t - rate_t0 >= 4.0) {
        double fps = (n_frames - rate_n0) / (t - rate_t0), best = fps;
        static const double R[] = {24, 25, 30, 48, 50, 60, 90, 100, 120};
        for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) if (fabs(fps - R[i]) < R[i] * 0.04) best = R[i];
        if (fps > 5) nom_dt = 1.0 / best;
        rate_t0 = t; rate_n0 = n_frames;
    }
    /* HLS: this frame ends the previous one */
    uint64_t rel, d = 0;
    if (!ts_started) { rel = 0; ts_started = 1; tl_t0 = t; }
    else { d = frame_dur(t, &tl_t0, last_ts); rel = last_ts + d; }

    /* live: the frame goes out immediately with its predicted duration; the timeline stays continuous */
    if (last_au_t > 0) arr_ms[arr_i++ % ARR_N] = (float)((t - last_au_t) * 1000.0);
    if (long_t0 == 0) { long_t0 = t; long_n0 = n_frames; }
    last_au_t = t;
    uint32_t ld = frame_dur(t, &live_t0, live_dts);
    emit_live(&au, au_key, live_dts, ld);
    live_dts += ld;

    /* HLS */
    if (open_seg && s_cnt > 0 && d) s_dur[s_cnt - 1] = (uint32_t)d;
    if (au_key && open_seg) {
        double seg_len = 0; for (int i = 0; i < s_cnt; i++) seg_len += s_dur[i];
        if (seg_len / TIMESCALE >= SEG_TARGET || s_cnt >= MAX_SAMPLES - 1) close_seg();
    }
    if (!open_seg && au_key) { open_seg = 1; s_base = rel; s_cnt = 0; sdata.n = 0; }
    if (open_seg && s_cnt < MAX_SAMPLES) {
        bput(&sdata, au.p, au.n);
        s_size[s_cnt] = au.n; s_dur[s_cnt] = TIMESCALE / 50; s_key[s_cnt] = au_key; s_cnt++;
    }
    last_ts = rel;
    n_frames++; n_bytes += au.n;
    au.n = 0; au_vcl = au_key = 0;
}

static void on_nal(const uint8_t *n, size_t len) {
    while (len > 0 && n[len - 1] == 0) len--;
    if (len < 1) return;
    unsigned t = n[0] & 0x1F;
    if (t == 7) {
        if (len <= sizeof(sps) && (len != sps_n || memcmp(n, sps, len))) {
            if (have_init) { push_au(); close_seg(); reset_stream("new SPS"); have_init = 0; }
            memcpy(sps, n, len); sps_n = len;
        }
        if (!have_init && pps_n) build_init();
        return;
    }
    if (t == 8) {
        if (len <= sizeof(pps) && (len != pps_n || memcmp(n, pps, len))) {
            memcpy(pps, n, len); pps_n = len;
            if (sps_n) { if (have_init) reset_stream("new PPS"); have_init = 0; build_init(); }
        }
        return;
    }
    if (t == 9) { push_au(); return; }
    if (t >= 1 && t <= 5) {
        if (len >= 2 && (n[1] & 0x80) && au_vcl) push_au();   /* first_mb_in_slice == 0 */
        au_vcl = 1; if (t == 5) au_key = 1;
    } else if (au_vcl) {
        push_au();
    }
    b32(&au, (uint32_t)len); bput(&au, n, len);
}

void hls_feed(const uint8_t *d, size_t n) {
    bput(&es, d, n);
    size_t i = 0, start = (size_t)-1;
    while (i + 3 <= es.n) {
        if (es.p[i] == 0 && es.p[i + 1] == 0 && es.p[i + 2] == 1) {
            if (start != (size_t)-1) on_nal(es.p + start, i - start);
            start = i + 3; i += 3;
        } else i++;
    }
    if (start == (size_t)-1) { if (es.n > 3) { memmove(es.p, es.p + es.n - 3, 3); es.n = 3; } return; }
    size_t keep = es.n - (start - 3);
    memmove(es.p, es.p + start - 3, keep); es.n = keep;
    if (es.n > 4 * 1024 * 1024) es.n = 0;
}

/* ---------------- page ---------------- */
static const char INDEX_HTML[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>metastreamer</title>"
"<style>html,body{margin:0;height:100%;background:#000}video{width:100%;height:100%;object-fit:contain}"
"#s{position:fixed;left:8px;top:8px;color:#8f8;font:12px monospace;text-shadow:0 0 3px #000}</style></head><body>"
"<video id=\"v\" autoplay muted playsinline></video><div id=\"s\">waiting for stream…</div><script>\n"
"const v=document.getElementById('v'),s=document.getElementById('s');let mode='',gen=0;\n"
"function sleep(ms){return new Promise(r=>setTimeout(r,ms));}\n"
"async function live(){const my=++gen;\n"
" const info=await (await fetch('info.json',{cache:'no-store'})).json();if(!info.ready)throw new Error('no stream');\n"
" const MS=window.ManagedMediaSource||window.MediaSource;const mime='video/mp4; codecs=\"'+info.codec+'\"';\n"
" if(!MS||!MS.isTypeSupported(mime))throw new Error('no MSE');\n"
" const ms=new MS();if(window.ManagedMediaSource)v.disableRemotePlayback=true;\n"
" v.src=URL.createObjectURL(ms);await new Promise(r=>ms.addEventListener('sourceopen',r,{once:true}));\n"
" const sb=ms.addSourceBuffer(mime);mode='live';const q=[];\n"
" const pump=()=>{if(sb.updating||!q.length||ms.readyState!=='open')return;\n"
"  const t=v.currentTime;if(sb.buffered.length&&t-sb.buffered.start(0)>10){sb.remove(0,t-4);return;}\n"
"  let n=0;for(const c of q)n+=c.length;const b=new Uint8Array(n);let o=0;for(const c of q){b.set(c,o);o+=c.length;}q.length=0;\n"
"  try{sb.appendBuffer(b);}catch(e){console.log(e);}};\n"
" sb.addEventListener('updateend',pump);\n"
" const r=(await fetch('live.mp4',{cache:'no-store'})).body.getReader();v.play().catch(()=>{});\n"
" for(;;){const {done,value}=await r.read();if(done||my!==gen)break;q.push(value);pump();}\n"
" throw new Error('stream ended');}\n"
"function hls(){mode='hls';v.src='stream.m3u8?t='+Date.now();v.play().catch(()=>{});}\n"
"async function run(){for(;;){try{await live();}catch(e){s.textContent='reconnecting: '+e.message;"
" if(e.message==='no MSE'&&v.canPlayType('application/vnd.apple.mpegurl')){hls();return;}}await sleep(1000);}}\n"
"setInterval(()=>{if(!v.buffered.length)return;const e=v.buffered.end(v.buffered.length-1),lag=e-v.currentTime;\n"
" if(mode==='live'){if(lag>0.6||v.currentTime<v.buffered.start(0))v.currentTime=e-0.1;"
"v.playbackRate=lag>0.25?1.08:lag>0.14?1.03:1.0;}else if(lag>1.2)v.currentTime=e-0.3;\n"
" if(v.paused)v.play().catch(()=>{});\n"
" s.textContent=mode+'  '+v.videoWidth+'x'+v.videoHeight+'  buffer '+(lag*1000|0)+' ms   '+tele;},200);\n"
"let tele='';const es=new EventSource('events');es.onmessage=e=>{try{const j=JSON.parse(e.data),t=j.telemetry||{},p=[];\n"
" if(t.link_quality!=null)p.push('link '+t.link_quality+'%');if(t.goggles_battery!=null)p.push('goggles '+t.goggles_battery+'%');\n"
" if(t.air_recorder&&t.air_recorder.recording)p.push('● REC '+t.air_recorder.record_secs+' s');\n"
" if(j.video)p.push(j.video.fps.toFixed(0)+' fps '+(j.video.kbps/1000).toFixed(1)+' Mbit/s');tele=p.join(' · ');}catch(x){}};\n"
"run();\n"
"</script></body></html>";

/* ---------------- HTTP ---------------- */
void hls_start(int port) {
    t_start = mono();
    for (int i = 0; i < MAX_CONN; i++) conns[i].fd = -1;
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lsock, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lsock, 8) < 0) { perror("http listen"); exit(1); }
    fcntl(lsock, F_SETFL, O_NONBLOCK);
    printf("[hls] http://<pi-ip>:%d/\n", port);
}

static void respond(Conn *c, int code, const char *type, const uint8_t *body, size_t blen) {
    size_t from = 0, to = blen ? blen - 1 : 0; int partial = 0;
    const char *rg = strstr(c->in, "Range: bytes=");
    if (!rg) rg = strstr(c->in, "range: bytes=");
    if (code == 200 && rg && blen) {
        unsigned long a = 0, b = 0; int k = sscanf(rg + 13, "%lu-%lu", &a, &b);
        if (k >= 1 && a < blen) { from = a; to = (k == 2 && b < blen) ? b : blen - 1; partial = 1; }
    }
    size_t len = blen ? to - from + 1 : 0;
    char hdr[640], cr[96] = "";
    if (partial) snprintf(cr, sizeof(cr), "Content-Range: bytes %zu-%zu/%zu\r\n", from, to, blen);
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
        "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n",
        partial ? "206 Partial Content" : code == 200 ? "200 OK" : code == 503 ? "503 Service Unavailable" :
        code == 504 ? "504 Gateway Timeout" : "404 Not Found", type, len, cr);
    c->out.n = 0; c->outpos = 0;
    bput(&c->out, hdr, hn);
    if (len) bput(&c->out, body + from, len);
    c->closing = 1;
}

/* headers complete and, for POST, the whole body (Content-Length) received */
static int request_complete(const Conn *c) {
    const char *end = strstr(c->in, "\r\n\r\n");
    if (!end) return 0;
    if (strncmp(c->in, "POST ", 5)) return 1;
    const char *cl = strcasestr(c->in, "\r\nContent-Length:");
    size_t len = cl && cl < end ? strtoul(cl + 17, NULL, 10) : 0;
    return c->inlen >= (size_t)(end + 4 - c->in) + len;
}

static void handle_request(Conn *c) {
    char method[8] = "GET", path[256] = "/";
    sscanf(c->in, "%7s %255s", method, path);
    int post = !strcmp(method, "POST");
    const char *body = strstr(c->in, "\r\n\r\n") + 4;
    char *q = strchr(path, '?'), *query = "";
    if (q) { *q = 0; query = q + 1; }
    n_req++;
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        respond(c, 200, "text/html; charset=utf-8", (const uint8_t *)INDEX_HTML, sizeof(INDEX_HTML) - 1);
    } else if (!strcmp(path, "/info.json")) {
        char js[160];
        int n = snprintf(js, sizeof(js), "{\"codec\":\"avc1.%02X%02X%02X\",\"width\":%d,\"height\":%d,\"ready\":%s}",
                         sps[1], sps[2], sps[3], vw, vh, have_init ? "true" : "false");
        respond(c, have_init ? 200 : 404, "application/json", (const uint8_t *)js, n);
    } else if (!strcmp(path, "/live.mp4")) {
        if (!have_init) { respond(c, 404, "text/plain", (const uint8_t *)"no stream\n", 10); return; }
        static const char h[] = "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nCache-Control: no-cache, no-store\r\n"
                                "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        c->out.n = 0; c->outpos = 0;
        bput(&c->out, h, sizeof(h) - 1);
        bput(&c->out, init_seg.p, init_seg.n);
        c->live = 1; c->wait_key = 1;
        int one = 1; setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    } else if (!strcmp(path, "/settings")) {
        respond(c, 200, "text/html; charset=utf-8", (const uint8_t *)SETTINGS_HTML, sizeof(SETTINGS_HTML) - 1);
    } else if (!strcmp(path, "/api/set")) {
        char key[32] = "", val[64] = "";
        sscanf(query, "%31[^=]=%63[^&]", key, val);
        url_decode(key); url_decode(val);
        int r = cam_fn ? cam_fn(key, val) : -3;
        static const char *err[] = {"ok", "invalid value", "unknown parameter", "no link to goggles"};
        char js[160];
        int n = snprintf(js, sizeof(js), "{\"ok\":%s,\"key\":\"%s\",\"error\":\"%s\"}",
                         r == 0 ? "true" : "false", key, err[r <= 0 && r >= -3 ? -r : 1]);
        respond(c, 200, "application/json", (const uint8_t *)js, n);
    } else if (!strcmp(path, "/api/wifi")) {
        static char js[4096];
        if (post) {
            const char *e = wifi_save(body);
            int n = snprintf(js, sizeof(js), "{\"ok\":%s,\"error\":", e ? "false" : "true");
            if (!e) n += snprintf(js + n, sizeof(js) - n, "null}");
            else {   /* the message may quote a network name */
                js[n++] = '"';
                for (const char *q2 = e; *q2 && n < (int)sizeof(js) - 4; q2++) {
                    if (*q2 == '"' || *q2 == '\\') js[n++] = '\\';
                    js[n++] = *q2;
                }
                n += snprintf(js + n, sizeof(js) - n, "\"}");
            }
            respond(c, 200, "application/json", (const uint8_t *)js, n);
        } else {
            size_t n = wifi_json(js, sizeof(js));
            respond(c, 200, "application/json", (const uint8_t *)js, n);
        }
    } else if (!strcmp(path, "/api/time")) {
        char js[320], tzv[96] = "";
        const char *e = NULL;
        if (post) {   /* tz=Europe%2FTallinn&auto=1 */
            int is_auto = conf_timezone_auto();
            char kv[128];
            for (const char *q2 = body; *q2;) {
                size_t l = strcspn(q2, "&");
                snprintf(kv, sizeof(kv), "%.*s", (int)(l < sizeof(kv) - 1 ? l : sizeof(kv) - 1), q2);
                q2 += l + (q2[l] == '&');
                if (!strncmp(kv, "tz=", 3)) { snprintf(tzv, sizeof(tzv), "%s", kv + 3); url_decode(tzv); }
                else if (!strncmp(kv, "auto=", 5)) is_auto = kv[5] == '1';
            }
            e = conf_set_timezone(tzv[0] ? tzv : conf_timezone(), is_auto);
        }
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        char loc[32]; strftime(loc, sizeof(loc), "%Y-%m-%dT%H:%M:%S", &tm);
        const char *z = conf_timezone();
        int n = snprintf(js, sizeof(js),
                         "{\"ok\":%s,\"error\":%s%s%s,\"timezone\":%s%s%s,\"auto\":%s,\"local\":\"%s\",\"ntp_synced\":%s}",
                         e ? "false" : "true", e ? "\"" : "", e ? e : "null", e ? "\"" : "",
                         z[0] ? "\"" : "", z[0] ? z : "null", z[0] ? "\"" : "",
                         conf_timezone_auto() ? "true" : "false", loc, access("/run/time-synced", F_OK) == 0 ? "true" : "false");
        respond(c, 200, "application/json", (const uint8_t *)js, n);
    } else if (!strcmp(path, "/api/net")) {
        static char js[1024];
        if (post) {   /* mode=static&address=192.168.1.50%2F24&gateway=...&dns=... */
            char kv[128], mode[16] = "", addr[32] = "", gw[32] = "", dns[32] = "";
            for (const char *q2 = body; *q2;) {
                size_t l = strcspn(q2, "&");
                snprintf(kv, sizeof(kv), "%.*s", (int)(l < sizeof(kv) - 1 ? l : sizeof(kv) - 1), q2);
                q2 += l + (q2[l] == '&');
                url_decode(kv);
                if (!strncmp(kv, "mode=", 5)) snprintf(mode, sizeof(mode), "%s", kv + 5);
                else if (!strncmp(kv, "address=", 8)) snprintf(addr, sizeof(addr), "%s", kv + 8);
                else if (!strncmp(kv, "gateway=", 8)) snprintf(gw, sizeof(gw), "%s", kv + 8);
                else if (!strncmp(kv, "dns=", 4)) snprintf(dns, sizeof(dns), "%s", kv + 4);
            }
            const char *e = conf_set_network(!strcmp(mode, "static"), addr, gw, dns);
            int n = snprintf(js, sizeof(js), "{\"ok\":%s,\"error\":%s%s%s}",
                             e ? "false" : "true", e ? "\"" : "", e ? e : "null", e ? "\"" : "");
            respond(c, 200, "application/json", (const uint8_t *)js, n);
        } else {
            size_t n = wifi_net_json(js, sizeof(js));
            respond(c, 200, "application/json", (const uint8_t *)js, n);
        }
    } else if (!strcmp(path, "/api/time/sync")) {
        static const char ok[] = "{\"ok\":true,\"error\":null}";
        static const char no[] = "{\"ok\":false,\"error\":\"the bridge clock is not synced yet (no NTP)\"}";
        int synced = access("/run/time-synced", F_OK) == 0;
        if (post && synced) conf_request_clock_sync();
        respond(c, 200, "application/json", (const uint8_t *)(synced ? ok : no), synced ? sizeof(ok) - 1 : sizeof(no) - 1);
    } else if (!strcmp(path, "/status")) {
        static char js[8192]; size_t n = status_json(js, sizeof(js));
        respond(c, 200, "application/json", (const uint8_t *)js, n);
    } else if (!strcmp(path, "/duml")) {
        static char js[65536]; size_t n = telem_duml_json(js, sizeof(js));
        respond(c, 200, "application/json", (const uint8_t *)js, n);
    } else if (!strcmp(path, "/events")) {
        static const char h[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                                "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        c->out.n = 0; c->outpos = 0; bput(&c->out, h, sizeof(h) - 1); c->sse = 1;
    } else if (!strcmp(path, "/api/duml/send")) {
        /* dst, set, id in hex; payload as hex digits; wait in s (default DUML_WAIT, max 5); the reply lists every frame from dst */
        unsigned dst = 0x3c, set = 0, id = 0; char hex[420] = ""; uint8_t pl[200]; size_t pn = 0; double wait = DUML_WAIT;
        char kv[460];
        for (const char *q2 = query; *q2;) {
            size_t l = strcspn(q2, "&");
            snprintf(kv, sizeof(kv), "%.*s", (int)(l < sizeof(kv) - 1 ? l : sizeof(kv) - 1), q2);
            q2 += l + (q2[l] == '&');
            if (!strncmp(kv, "dst=", 4)) dst = strtoul(kv + 4, NULL, 16);
            else if (!strncmp(kv, "set=", 4)) set = strtoul(kv + 4, NULL, 16);
            else if (!strncmp(kv, "id=", 3)) id = strtoul(kv + 3, NULL, 16);
            else if (!strncmp(kv, "payload=", 8)) snprintf(hex, sizeof(hex), "%s", kv + 8);
            else if (!strncmp(kv, "wait=", 5)) wait = atof(kv + 5);
        }
        for (const char *h = hex; h[0] && h[1] && pn < sizeof(pl); h += 2) { unsigned v; if (sscanf(h, "%2x", &v) != 1) break; pl[pn++] = (uint8_t)v; }
        if (!duml_send_fn) {
            static const char off[] = "disabled: start metastreamer with --duml-console\n";
            respond(c, 404, "text/plain", (const uint8_t *)off, sizeof(off) - 1); return;
        }
        if (duml_send_fn((uint8_t)dst, (uint8_t)set, (uint8_t)id, pl, pn) != 0) {
            respond(c, 503, "application/json", (const uint8_t *)"{\"ok\":false,\"error\":\"no link to goggles\"}", 41); return;
        }
        c->duml_wait = 1; c->duml_since = telem_raw_seq(); c->duml_src = (uint8_t)dst; c->duml_t0 = mono();
        c->duml_for = wait < 0.1 ? 0.1 : wait > 5 ? 5 : wait;
    } else if (!strcmp(path, "/snapshot") || !strcmp(path, "/snapshot.jpg")) {
        if (!hdmi_decoder_running()) { respond(c, 503, "text/plain", (const uint8_t *)"no decoder\n", 11); return; }
        size_t n; unsigned sq; double age;
        const uint8_t *j = hdmi_snapshot_lock(&n, &sq, &age);
        if (n && age < 0.15) { respond(c, 200, "image/jpeg", j, n); hdmi_snapshot_unlock(); return; }
        hdmi_snapshot_unlock();
        hdmi_snapshot_request();
        c->snap_wait = 1; c->snap_seq0 = sq; c->snap_t0 = mono();
    } else if (!strcmp(path, "/stream.m3u8")) {
        if (!have_init || ring_n == 0) { respond(c, 404, "text/plain", (const uint8_t *)"no stream\n", 10); return; }
        char pl[8192]; int first = ring_n > LIST ? ring_n - LIST : 0; double maxd = 1;
        for (int i = first; i < ring_n; i++) if (ring[i].dur > maxd) maxd = ring[i].dur;
        int n = snprintf(pl, sizeof(pl),
            "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:%u\n"
            "#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MAP:URI=\"init.mp4\"\n",
            (int)(maxd + 0.999), ring[first].seq);
        for (int i = first; i < ring_n && n < (int)sizeof(pl) - 400; i++) {
            char iso[40]; time_t sec = (time_t)ring[i].rt; struct tm tm; gmtime_r(&sec, &tm);
            size_t k = strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
            snprintf(iso + k, sizeof(iso) - k, ".%03dZ", (int)((ring[i].rt - (double)sec) * 1000));
            n += snprintf(pl + n, sizeof(pl) - n, "#EXT-X-PROGRAM-DATE-TIME:%s\n", iso);
            if (ring[i].meta[0])
                n += snprintf(pl + n, sizeof(pl) - n, "#EXT-X-DATERANGE:ID=\"seg%u\",START-DATE=\"%s\",DURATION=%.3f%s\n",
                              ring[i].seq, iso, ring[i].dur, ring[i].meta);
            n += snprintf(pl + n, sizeof(pl) - n, "#EXTINF:%.3f,\nseg_%u.m4s\n", ring[i].dur, ring[i].seq);
        }
        respond(c, 200, "application/vnd.apple.mpegurl", (const uint8_t *)pl, n);
    } else if (!strcmp(path, "/init.mp4")) {
        if (!have_init) respond(c, 404, "text/plain", (const uint8_t *)"no init\n", 8);
        else respond(c, 200, "video/mp4", init_seg.p, init_seg.n);
    } else {
        unsigned s;
        if (sscanf(path, "/seg_%u.m4s", &s) == 1)
            for (int i = 0; i < ring_n; i++) if (ring[i].seq == s) { respond(c, 200, "video/mp4", ring[i].data.p, ring[i].data.n); return; }
        respond(c, 404, "text/plain", (const uint8_t *)"not found\n", 10);
    }
}

int hls_pollfds(struct pollfd *p, int max) {
    int k = 0;
    if (k < max) { p[k].fd = lsock; p[k].events = POLLIN; p[k].revents = 0; k++; }
    int wf = hdmi_snapshot_wake_fd();
    if (wf >= 0 && k < max) { p[k].fd = wf; p[k].events = POLLIN; p[k].revents = 0; k++; }
    double t = mono();
    for (int i = 0; i < MAX_CONN && k < max; i++) {
        Conn *c = &conns[i];
        if (c->fd < 0) continue;
        if (!c->live && !c->sse && !c->snap_wait && !c->duml_wait && t - c->t > 15) { conn_close(c); continue; }
        int pending = c->outpos < c->out.n;
        p[k].fd = c->fd; p[k].events = pending ? POLLOUT : POLLIN; p[k].revents = 0; k++;
    }
    return k;
}

void hls_handle(const struct pollfd *p, int n) {
    for (int k = 0; k < n; k++) {
        if (!p[k].revents) continue;
        if (p[k].fd == hdmi_snapshot_wake_fd()) { char tmp[64]; while (read(p[k].fd, tmp, sizeof(tmp)) > 0) {} continue; }
        if (p[k].fd == lsock) {
            for (;;) {
                int fd = accept(lsock, NULL, NULL);
                if (fd < 0) break;
                fcntl(fd, F_SETFL, O_NONBLOCK);
                int i; for (i = 0; i < MAX_CONN && conns[i].fd >= 0; i++) {}
                if (i == MAX_CONN) { close(fd); continue; }
                Conn *c = &conns[i];
                c->fd = fd; c->inlen = 0; c->out.n = 0; c->outpos = 0; c->t = mono(); c->live = c->closing = c->wait_key = c->sse = c->snap_wait = c->duml_wait = 0;
            }
            continue;
        }
        Conn *c = NULL;
        for (int i = 0; i < MAX_CONN; i++) if (conns[i].fd == p[k].fd) { c = &conns[i]; break; }
        if (!c) continue;
        if (p[k].revents & (POLLERR | POLLNVAL)) { conn_close(c); continue; }
        if ((p[k].revents & POLLOUT) && c->outpos < c->out.n) {
            ssize_t w = send(c->fd, c->out.p + c->outpos, c->out.n - c->outpos, MSG_NOSIGNAL);
            if (w < 0) { if (errno == EAGAIN || errno == EINTR) continue; conn_close(c); continue; }
            c->outpos += w; c->t = mono();
            if (c->outpos >= c->out.n) {
                if (c->closing) { conn_close(c); continue; }
                c->out.n = 0; c->outpos = 0;   /* live: wait for the next frame */
            }
            continue;
        }
        if (p[k].revents & (POLLIN | POLLHUP)) {
            if (c->live || c->sse || c->snap_wait || c->duml_wait) {   /* no incoming data expected; 0 = peer closed */
                char tmp[256]; ssize_t r = recv(c->fd, tmp, sizeof(tmp), 0);
                if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) conn_close(c);
                continue;
            }
            ssize_t r = recv(c->fd, c->in + c->inlen, sizeof(c->in) - 1 - c->inlen, 0);
            if (r <= 0) { if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue; conn_close(c); continue; }
            c->inlen += r; c->in[c->inlen] = 0; c->t = mono();
            if (request_complete(c)) handle_request(c);
            else if (c->inlen >= sizeof(c->in) - 1) conn_close(c);
        }
    }
}

static void queue_out(Conn *c, const void *d, size_t n) {
    if (c->outpos > 0 && c->outpos == c->out.n) { c->out.n = 0; c->outpos = 0; }
    bput(&c->out, d, n);
}

void hls_tick(void) {
    double t = mono();
    if (t - win_t >= 1.0) {
        if (win_t > 0) { double dt = t - win_t; rate_fps = (n_frames - win_frames) / dt; rate_kbps = (n_bytes - win_bytes) * 8.0 / dt / 1000.0; }
        win_t = t; win_frames = n_frames; win_bytes = n_bytes;
    }
    static double last_sse;
    int any_sse = 0;
    for (int i = 0; i < MAX_CONN; i++) any_sse |= conns[i].fd >= 0 && conns[i].sse;
    if (any_sse && t - last_sse >= SSE_PERIOD) {
        static char js[8192], msg[8300];
        size_t n = status_json(js, sizeof(js));
        int m = snprintf(msg, sizeof(msg), "data: %.*s\n\n", (int)n, js);
        for (int i = 0; i < MAX_CONN; i++) {
            Conn *c = &conns[i];
            if (c->fd < 0 || !c->sse) continue;
            if (c->out.n - c->outpos > 256 * 1024) { conn_close(c); continue; }
            queue_out(c, msg, m);
        }
        last_sse = t;
    }
    for (int i = 0; i < MAX_CONN; i++) {
        Conn *c = &conns[i];
        if (c->fd < 0 || !c->duml_wait || t - c->duml_t0 < c->duml_for) continue;
        static char js[16384];
        size_t n = telem_raw_json(c->duml_since, c->duml_src, js, sizeof(js));
        c->duml_wait = 0;
        respond(c, 200, "application/json", (const uint8_t *)js, n);
    }
    for (int i = 0; i < MAX_CONN; i++) {
        Conn *c = &conns[i];
        if (c->fd < 0 || !c->snap_wait) continue;
        size_t n; unsigned sq; double age;
        const uint8_t *j = hdmi_snapshot_lock(&n, &sq, &age);
        if (n && sq != c->snap_seq0) { c->snap_wait = 0; respond(c, 200, "image/jpeg", j, n); }
        hdmi_snapshot_unlock();
        if (c->snap_wait && t - c->snap_t0 > SNAP_TIMEOUT) {
            c->snap_wait = 0; respond(c, 504, "text/plain", (const uint8_t *)"no frame\n", 9);
        }
    }
}

void hls_stats(char *out, size_t cap) {
    int nl = 0; for (int i = 0; i < MAX_CONN; i++) nl += conns[i].fd >= 0 && conns[i].live;
    snprintf(out, cap, "%dx%d frames=%llu segs=%llu live_clients=%d live_frags=%llu resets=%llu req=%llu",
             vw, vh, n_frames, n_segs, nl, n_live_frags, n_resets, n_req);
}
