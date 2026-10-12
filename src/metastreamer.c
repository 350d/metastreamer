/*
 * metastreamer: DJI Goggles 2/3/N3 -> browser and HDMI bridge, for a Raspberry Pi Zero in USB gadget mode.
 * LogicLink/DUML protocol from YStreamer; single-threaded poll() loop (HDMI runs in its own thread).
 *
 * Outputs: http://<pi>/            player page (Safari: per-frame fMP4 via MediaSource, <1 s)
 *          http://<pi>/live.mp4    endless per-frame fMP4 (low latency)
 *          http://<pi>/stream.m3u8 HLS (fallback)
 *          HDMI                    hardware decode, on by default (--no-hdmi turns it off)
 *
 * Run (the tty must be in raw mode; the launch command takes care of that):
 *   sh -c 'exec 3<>/dev/ttyGS0; stty -F /dev/ttyGS0 raw -echo; exec ./metastreamer [--http 80] [--no-hdmi]'
 *
 * Build (on the Mac):
 *   zig cc -target arm-linux-musleabihf -mcpu=arm1176jzf_s -O2 -static -s -o metastreamer metastreamer.c hls.c hdmi.c
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hdmi.h"
#include "hls.h"
#include "telem.h"
#include "wifi.h"
#include "conf.h"

#define DEV "/dev/ttyGS0"
#define APP 0x02
#define FLAGS_REQUEST 0x40
#define FLAGS_RESPONSE 0x80
#define VIDEO_PORT 0x574A
#define VIDEO_PORT_B 0x574B
#define TOPIC_HOST 0x28
#define REGISTRAR 0x3C
#define CMD_GENERAL 0x00
#define CMD_IDENTITY 0x81
#define CMD_IDENTITY_ACK 0x82
#define CMD_LINK 0x88
#define CMD_TOPIC 0x99
#define OP_HEARTBEAT 0x19
#define OP_HEARTBEAT_ACK 0x1a
#define CMD_SET_DATE 0x4A   /* dji_general_set_device_date: year u16, month, day, hour, min, sec (local time) */
#define CMD_GET_DATE 0x4B   /* dji_general_get_device_date; reply: status, year u16, month, day, hour, min, sec */
#define TIME_SYNCED "/run/time-synced"   /* created by the ntpd hook (metastreamer-ntp-hook) */

#define BUF_CAP (1024 * 1024)

static int tty = -1;
static uint8_t buf[BUF_CAP];
static size_t blen;
static uint16_t seq = 1;
static int registered;
static double last_hb, last_reg, started;
static unsigned long long vbytes, port_bytes_video, port_bytes_other;
static char goggles_clock[24];   /* last answer to CMD_GET_DATE, "YYYY-MM-DDTHH:MM:SS" */
static time_t goggles_t;         /* that answer (local time, see conf.c) as epoch seconds ... */
static double goggles_rx;        /* ... and when it arrived (monotonic) */

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- CRC ---- */
static uint8_t crc8(const uint8_t *d, size_t n) {
    unsigned c = 0x77;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8C : (c >> 1);
    }
    return c & 0xFF;
}

static uint16_t crc16(const uint8_t *d, size_t n) {
    unsigned c = 0x3692;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8408 : (c >> 1);
    }
    return c & 0xFFFF;
}

/* ---- codec: DUML in a LogicLink wrapper (55 CC 49 57 + u32 length) ---- */
static void send_duml(uint8_t dst, uint16_t s, uint8_t flags, uint8_t cmd_set, uint8_t cmd_id,
                      const uint8_t *payload, size_t plen) {
    uint8_t out[512];
    size_t duml_len = 11 + plen + 2;
    if (8 + duml_len > sizeof(out)) return;
    size_t o = 0;
    out[o++] = 0x55; out[o++] = 0xCC; out[o++] = 0x49; out[o++] = 0x57;
    out[o++] = duml_len & 0xFF; out[o++] = (duml_len >> 8) & 0xFF; out[o++] = 0; out[o++] = 0;
    size_t start = o;
    out[o++] = 0x55;
    out[o++] = duml_len & 0xFF;
    out[o++] = ((duml_len >> 8) & 0x03) | (1 << 2);
    out[o] = crc8(out + start, 3); o++;
    out[o++] = APP;
    out[o++] = dst;
    out[o++] = s & 0xFF; out[o++] = s >> 8;
    out[o++] = flags;
    out[o++] = cmd_set;
    out[o++] = cmd_id;
    if (plen) memcpy(out + o, payload, plen);
    o += plen;
    uint16_t c = crc16(out + start, o - start);
    out[o++] = c & 0xFF; out[o++] = c >> 8;
    if (tty >= 0 && write(tty, out, o) < 0) perror("tty write");
}

static void request(uint8_t dst, uint8_t cmd_id, const uint8_t *p, size_t n) {
    send_duml(dst, seq++, FLAGS_REQUEST, CMD_GENERAL, cmd_id, p, n);
}

/* ---- registration packets (from YStreamer session.rs) ---- */
static void do_register(void) {
    uint8_t topic[13 + 2 + 13 + 4] = {0x02,0x02,0x00,0x00,0xd5,0x07,0x00,0x00,0x00,0x00,0x00,0x13,0x00};
    const char *name = "camcap_common";
    topic[13] = 13; topic[14] = 0;
    memcpy(topic + 15, name, 13);
    memset(topic + 28, 0, 4);
    request(TOPIC_HOST, CMD_TOPIC, topic, sizeof(topic));

    uint8_t reg[14] = {0x17,0x00,0x00,0x23,0x00,'A','P','P',0,0,0,0,0,0x02};
    request(REGISTRAR, CMD_LINK, reg, sizeof(reg));

    const char *ver = "1.21.1";
    uint8_t v[8 + 2 + 6] = {0x1d,0x00,0x01,0x00,0x00,0x00,0x00,0x01};
    v[8] = (uint8_t)(strlen(ver) + 1); v[9] = 0;
    memcpy(v + 10, ver, 6);
    request(REGISTRAR, CMD_LINK, v, sizeof(v));
}

static void on_video(const uint8_t *d, size_t n) {
    vbytes += n;
    hls_feed(d, n);
}

/* ---- clock: the goggles keep the date the DJI Fly app last set. Before NTP has synced, it is used only
 * when it is newer than the system clock (which starts at the image build time), so a stale goggles clock
 * changes nothing. After NTP has synced, the bridge sets the goggles' clock instead, as DJI Fly does,
 * so their recordings get the right date. ---- */
static void on_goggles_date(const uint8_t *d) {
    struct tm tm = {0};
    tm.tm_year = (d[0] | d[1] << 8) - 1900; tm.tm_mon = d[2] - 1; tm.tm_mday = d[3];
    tm.tm_hour = d[4]; tm.tm_min = d[5]; tm.tm_sec = d[6];
    if (tm.tm_year < 120 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 || d[4] > 23 || d[5] > 59 || d[6] > 60) return;
    int first = !goggles_clock[0];
    goggles_rx = now();
    snprintf(goggles_clock, sizeof(goggles_clock), "%04d-%02d-%02dT%02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    tm.tm_isdst = -1;
    time_t g = mktime(&tm);   /* the goggles keep local time (the bridge sets it so) */
    goggles_t = g;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (access(TIME_SYNCED, F_OK) == 0) {
        if (first) printf("[time] goggles clock %s, system clock is synced\n", goggles_clock);
    } else if (g > tv.tv_sec + 60) {
        struct timeval nt = {g, 0};
        if (settimeofday(&nt, NULL) == 0) printf("[time] clock set from the goggles: %s local\n", goggles_clock);
        else perror("settimeofday");
    } else if (first) {
        printf("[time] goggles clock %s is older than the system clock, not used\n", goggles_clock);
    }
}

static void clock_poll(void) {
    static double last_get, last_set, reget_at; static int gets, sets, force; static unsigned conf_gen, sync_gen;
    double t = now();
    if (!registered) return;
    /* time zone changed (or first pass): the goggles' wall clock was read in the old zone, so set it anew */
    if (conf_gen != conf_generation()) { conf_gen = conf_generation(); sets = 0; force = 1; last_set = 0; }
    if (sync_gen != conf_clock_sync_generation()) { sync_gen = conf_clock_sync_generation(); sets = 0; force = 1; last_set = 0; }
    if (!goggles_clock[0]) {   /* first read */
        if (gets < 5 && t - last_get >= 3) { last_get = t; gets++; request(REGISTRAR, CMD_GET_DATE, NULL, 0); }
        return;
    }
    if (reget_at && t >= reget_at) { reget_at = 0; request(REGISTRAR, CMD_GET_DATE, NULL, 0); return; }
    if (reget_at || sets >= 3 || t - last_set < 5 || access(TIME_SYNCED, F_OK) != 0) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double off = (double)tv.tv_sec + tv.tv_usec / 1e6 - ((double)goggles_t + (t - goggles_rx));
    if (!force && off > -5 && off < 5) {
        if (sets) { printf("[time] goggles clock set (now %+.1f s off)\n", off); sets = 3; }
        return;
    }
    force = 0;
    if (sets) printf("[time] goggles clock still %+.0f s off\n", off);
    if (++sets > 3) return;
    struct tm tm;
    time_t s = tv.tv_sec + (tv.tv_usec >= 500000);
    localtime_r(&s, &tm);
    uint8_t p[7] = {(uint8_t)((tm.tm_year + 1900) & 0xFF), (uint8_t)((tm.tm_year + 1900) >> 8), (uint8_t)(tm.tm_mon + 1),
                    (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec};
    printf("[time] setting the goggles clock (%+.0f s off) to %04d-%02d-%02dT%02d:%02d:%02d %s\n", off,
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
           conf_timezone()[0] ? conf_timezone() : "UTC");
    request(REGISTRAR, CMD_SET_DATE, p, sizeof(p));
    last_set = t;
    reget_at = t + 1.5;
}

/* ---- handling DUML from the goggles ---- */
static void on_duml(const uint8_t *in, size_t len) {
    uint8_t src = in[4], flags = in[8], cmd_set = in[9], cmd_id = in[10];
    uint16_t s = in[6] | (in[7] << 8);
    const uint8_t *pl = in + 11;
    size_t plen = len - 13;
    int is_resp = flags & 0x80;
    int wants = !is_resp && ((flags >> 5) & 0x03);
    static const uint8_t zero[1] = {0};

    if (src == 2 || src == 5) return;
    telem_on_duml(src, in[5], flags, cmd_set, cmd_id, pl, plen);
    if (cmd_set != CMD_GENERAL) {
        if (wants) send_duml(src, s, FLAGS_RESPONSE, cmd_set, cmd_id, zero, 1);
        return;
    }
    if (is_resp) {
        if (cmd_id == CMD_GET_DATE && plen >= 8 && pl[0] == 0) on_goggles_date(pl + 1);
        if (cmd_id == CMD_SET_DATE) printf("[time] set-date answer from 0x%02x: status %u, %zu bytes\n", src, plen ? pl[0] : 255, plen);
        return;
    }
    int hb = cmd_id == CMD_LINK && plen >= 1 && pl[0] == OP_HEARTBEAT;
    if (hb) { last_hb = now(); registered = 1; }
    if (!wants) return;

    if (cmd_id == CMD_IDENTITY) {
        uint8_t r[1 + 32 + 8 + 8];
        memset(r, 0, sizeof(r));
        memcpy(r + 1, "APP", 3);
        r[33 + 1] = 0x02;
        if (plen >= 48) memcpy(r + 41, pl + 40, 8);
        else { r[41] = 0x05; r[42] = 0x1c; }
        send_duml(src, s, FLAGS_RESPONSE, cmd_set, cmd_id, r, sizeof(r));
    } else if (hb) {
        uint8_t r[5] = {OP_HEARTBEAT_ACK, 0, 0, 0, 0};
        send_duml(src, s, FLAGS_RESPONSE, cmd_set, cmd_id, r, sizeof(r));
    } else {
        send_duml(src, s, FLAGS_RESPONSE, cmd_set, cmd_id, zero, 1);
    }
}

/* ---- LogicLink stream parsing ---- */
static void process(void) {
    size_t i = 0;
    while (i + 8 <= blen) {
        if (buf[i] != 0x55 || buf[i + 1] != 0xCC) { i++; continue; }
        unsigned port = buf[i + 2] | (buf[i + 3] << 8);
        size_t ilen = buf[i + 4] | (buf[i + 5] << 8);
        size_t total = 8 + ilen;
        if (i + total > blen) break;
        const uint8_t *in = buf + i + 8;
        if (port == VIDEO_PORT || port == VIDEO_PORT_B) {
            port_bytes_video += ilen;
            if (ilen >= 13 && in[0] == 0x55) {
                size_t dl = in[1] | ((in[2] & 0x03) << 8);
                if (dl == ilen) { if (ilen > 13) on_video(in + 11, ilen - 13); }
                else on_video(in, ilen);
            } else {
                on_video(in, ilen);
            }
        } else {
            port_bytes_other += ilen;
            if (ilen >= 13 && in[0] == 0x55) {
                size_t dl = in[1] | ((in[2] & 0x03) << 8);
                if (dl == ilen && crc8(in, 3) == in[3]) on_duml(in, ilen);
            }
        }
        i += total;
    }
    if (i > 0) { memmove(buf, buf + i, blen - i); blen -= i; }
}

/* ---- air unit camera: settings (command format from YStreamer camera.rs) ---- */
#define CAM_ADDR 0x01
#define CAM_SET 0x02
static void cam_send(uint8_t id, const uint8_t *p, size_t n) { send_duml(CAM_ADDR, seq++, FLAGS_REQUEST, CAM_SET, id, p, n); }
static const uint8_t CAM_POLLED[] = {24, 30, 40, 42, 44, 46, 56, 66, 68, 70};

/* 0 = sent; -1 = invalid value; -2 = unknown parameter; -3 = no link to goggles */
static int cam_set(const char *k, const char *v) {
    static const int iso_tab[] = {25, 50, 100, 200, 400, 800, 1600, 3200, 6400, 12800, 25600};
    static const int shut_tab[] = {8000, 6400, 5000, 4000, 3200, 2500, 2000, 1600, 1250, 1000, 800, 640, 500,
                                   400, 320, 250, 240, 200, 160, 120, 100, 80, 60, 50, 40, 30};
    static const int ev10_tab[] = {-30, -27, -23, -20, -17, -13, -10, -7, -3, 0, 3, 7, 10, 13, 17, 20, 23, 27, 30};
    uint8_t b[8]; size_t n = 0; uint8_t id;
    if (!registered) return -3;
    if (!strcmp(k, "format")) {
        static const struct { const char *r, *a; uint8_t w; } T[] = {
            {"1080p", "16:9", 10}, {"1080p", "4:3", 98}, {"2.7K", "16:9", 45}, {"2.7K", "4:3", 95}, {"4K", "16:9", 16}, {"4K", "4:3", 103}};
        char r[16], a[8]; int fps;
        if (sscanf(v, "%15[^,],%7[^,],%d", r, a, &fps) != 3) return -1;
        int fw = telem_fps_wire(fps);
        if (fw < 0) return -1;
        size_t i; for (i = 0; i < sizeof(T) / sizeof(T[0]) && (strcmp(T[i].r, r) || strcmp(T[i].a, a)); i++) {}
        if (i == sizeof(T) / sizeof(T[0])) return -1;
        id = 24; b[0] = T[i].w; b[1] = (uint8_t)fw; b[2] = 0; n = 3;
    } else if (!strcmp(k, "exposure")) {
        id = 30; n = 2; b[1] = 0;
        if (!strcmp(v, "auto")) b[0] = 0x01; else if (!strcmp(v, "manual")) b[0] = 0x04; else return -1;
    } else if (!strcmp(k, "shutter")) {
        int sp = atoi(v); id = 40; n = 4;
        if (sp == 0) { b[0] = 1; b[1] = b[2] = b[3] = 0; }
        else {
            size_t i; for (i = 0; i < sizeof(shut_tab) / sizeof(int) && shut_tab[i] != sp; i++) {}
            if (i == sizeof(shut_tab) / sizeof(int)) return -1;
            unsigned d = 32768 + sp; b[0] = 1; b[1] = d & 0xFF; b[2] = d >> 8; b[3] = 0;
        }
    } else if (!strcmp(k, "iso")) {
        int iso = atoi(v); id = 42; n = 1;
        if (iso == 0) b[0] = 0;
        else { size_t i; for (i = 0; i < 11 && iso_tab[i] != iso; i++) {} if (i == 11) return -1; b[0] = (uint8_t)(i + 1); }
    } else if (!strcmp(k, "ev")) {
        int e10 = (int)(atof(v) * 10 + (atof(v) < 0 ? -0.5 : 0.5)); id = 46; n = 1;
        size_t i; for (i = 0; i < 19 && ev10_tab[i] != e10; i++) {} if (i == 19) return -1;
        b[0] = (uint8_t)(i + 7);
    } else if (!strcmp(k, "wb")) {
        id = 44;
        if (!strcmp(v, "auto")) { b[0] = 0; b[1] = 0; n = 2; }
        else { int tk = atoi(v); if (tk < 2000 || tk > 10000) return -1; unsigned q = tk / 100;
               b[0] = 6; b[1] = q & 0xFF; b[2] = q >> 8; b[3] = 0xFF; b[4] = 0xFF; n = 5; }
    } else if (!strcmp(k, "sharpness")) {
        int x = atoi(v); if (x < -3 || x > 3) return -1; id = 56; b[0] = (uint8_t)(int8_t)x; n = 1;
    } else if (!strcmp(k, "nr")) {
        int x = atoi(v); if (x < -2 || x > 1) return -1; id = 68; b[0] = (uint8_t)(int8_t)x; n = 1;
    } else if (!strcmp(k, "color")) {
        id = 66; n = 1;
        if (!strcmp(v, "normal")) b[0] = 0x00; else if (!strcmp(v, "dcinelike")) b[0] = 0x06; else if (!strcmp(v, "dlog")) b[0] = 0x3D; else return -1;
    } else if (!strcmp(k, "flicker")) {
        id = 70; n = 1;
        if (!strcmp(v, "auto")) b[0] = 0; else if (!strcmp(v, "60hz")) b[0] = 1; else if (!strcmp(v, "50hz")) b[0] = 2; else if (!strcmp(v, "off")) b[0] = 3; else return -1;
    } else return -2;
    printf("[cam] %s=%s\n", k, v);
    cam_send(id, b, n);
    cam_send(id + 1, NULL, 0);   /* read it back right away */
    return 0;
}

static void cam_poll(void) {
    static double last; static unsigned i;
    double t = now();
    if (t - last < 0.2) return;
    last = t;
    if (!registered || !telem_camera_needs_poll()) return;
    cam_send(CAM_POLLED[i++ % (sizeof(CAM_POLLED))] + 1, NULL, 0);
}

/* --duml-probe SET:ID: research aid. For 30 s after registration, every 3 s send this request with an empty
 * payload to every address seen in the goggles' traffic; replies show up in /duml. */
static int probe_set = -1, probe_id;
static void duml_probe(void) {
    static const uint8_t dst[] = {0x01, 0x09, 0x0E, 0x1C, 0x28, 0x29, 0x2E, 0x3C, 0x48, 0xBC};
    static double first, last;
    if (probe_set < 0 || !registered) return;
    double t = now();
    if (!first) first = t;
    if (t - first > 30 || t - last < 3) return;
    last = t;
    for (size_t i = 0; i < sizeof(dst); i++) send_duml(dst[i], seq++, FLAGS_REQUEST, (uint8_t)probe_set, (uint8_t)probe_id, NULL, 0);
    printf("[probe] %02x:%02x sent to %zu addresses\n", probe_set, probe_id, sizeof(dst));
}

/* /api/duml/send: one request on the goggles' bus; -1 = not registered */
static int duml_console_send(uint8_t dst, uint8_t set, uint8_t id, const uint8_t *pl, size_t n) {
    if (!registered) return -1;
    send_duml(dst, seq++, FLAGS_REQUEST, set, id, pl, n);
    return 0;
}

static size_t link_json(char *o, size_t cap) {
    int n = snprintf(o, cap, "{\"registered\":%s,\"video_bytes\":%llu,\"duml_bytes\":%llu,\"goggles_clock\":%s%s%s}",
                     registered ? "true" : "false", port_bytes_video, port_bytes_other,
                     goggles_clock[0] ? "\"" : "", goggles_clock[0] ? goggles_clock : "null", goggles_clock[0] ? "\"" : "");
    return n < (int)cap ? (size_t)n : cap - 1;
}

/* The host hung up the tty: the cable was pulled. On replug the goggles see a fresh device,
 * so metastreamer-reconnect must not run its slow off-bus sequence before the next start. */
static void mark_unplugged(void) {
    int fd = open("/run/metastreamer.unplugged", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
}

int main(int argc, char **argv) {
    /* boot-time helper for metastreamer-wifi: [wifi] in metastreamer.txt -> wpa_supplicant.conf */
    if (argc == 4 && !strcmp(argv[1], "--wifi-conf")) return wifi_gen_conf(argv[2], argv[3]);
    int http_port = 80, use_hdmi = 1, duml_console = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-hdmi")) use_hdmi = 0;
        else if (!strcmp(argv[i], "--http") && i + 1 < argc) http_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hdmi-mode") && i + 1 < argc && hdmi_set_mode(argv[i + 1]) == 0) i++;
        else if (!strcmp(argv[i], "--duml-probe") && i + 1 < argc && sscanf(argv[i + 1], "%x:%x", &probe_set, &probe_id) == 2) i++;
        else if (!strcmp(argv[i], "--duml-console")) duml_console = 1;
        else { fprintf(stderr, "usage: %s [--http PORT] [--no-hdmi] [--hdmi-mode WxH[@HZ]|auto] [--duml-probe SET:ID] [--duml-console]\n"
                               "       %s --wifi-conf metastreamer.txt wpa_supplicant.conf\n"
                               "  --no-hdmi: no display, decoding for /snapshot stays on\n"
                               "  --hdmi-mode: default 1920x1080 (50 Hz, else 60); auto = display's preferred mode\n"
                               "  --duml-probe: research, send request SET:ID (hex) to all known addresses, replies in /duml\n"
                               "  --duml-console: research, enable /api/duml/send (any request to any device, no login: never on an open network)\n",
                               argv[0], argv[0]); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    conf_load();
    setvbuf(stdout, NULL, _IOLBF, 0);

    tty = open(DEV, O_RDWR | O_NOCTTY);
    if (tty < 0) { perror(DEV); return 1; }
    hls_start(http_port);
    hls_set_link_fn(link_json);
    hls_set_camera_fn(cam_set);
    if (duml_console) hls_set_duml_fn(duml_console_send);
    if (hdmi_start(use_hdmi) == 0) hls_set_au_sink(hdmi_feed_au);   /* always decode (for /snapshot), display depends on the flag */

    started = now();
    double last_stats = started, last_rx = started;
    for (;;) {
        struct pollfd p[1 + 24] = {{tty, POLLIN, 0}};
        int nh = hls_pollfds(p + 1, 24);
        int r = poll(p, 1 + nh, 250);
        if (r < 0 && errno != EINTR) { perror("poll"); break; }

        if (r > 0 && (p[0].revents & (POLLHUP | POLLERR))) {
            fprintf(stderr, "tty: link to goggles lost (cable pulled?) — exiting, systemd will restart\n");
            mark_unplugged();
            return 3;
        }
        if (r > 0 && (p[0].revents & POLLIN)) {
            if (blen >= BUF_CAP) blen = 0; /* garbage without headers — reset */
            ssize_t n = read(tty, buf + blen, BUF_CAP - blen);
            if (n > 0) { blen += (size_t)n; last_rx = now(); process(); }
            else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                fprintf(stderr, "tty: read error — exiting, systemd will restart\n");
                mark_unplugged();
                return 3;
            }
        }
        if (r > 0) hls_handle(p + 1, nh);
        hls_tick();
        cam_poll();
        clock_poll();
        duml_probe();

        double t = now();
        if (t - last_rx > 10.0) {
            fprintf(stderr, "tty: goggles silent for 10 s — exiting, systemd will restart with a reconnect\n");
            return 4;
        }
        int alive = registered && (t - last_hb < 5.0);
        if (!alive && t - last_reg >= 1.0) {
            if (registered) registered = 0;
            do_register();
            last_reg = t;
        }
        if (t - last_stats >= 5.0) {
            char hs[256], ds[160];
            hls_stats(hs, sizeof(hs)); hdmi_stats(ds, sizeof(ds));
            printf("[fwd] reg=%d video=%.1fMB ~%.0fKB/s ports video=%llu other=%llu | %s | %s\n",
                   registered, vbytes / 1048576.0, vbytes / (t - started) / 1024.0,
                   port_bytes_video, port_bytes_other, hs, ds);
            last_stats = t;
        }
    }
    return 0;
}
