/*
 * jpeg.c — a small baseline JPEG encoder (YCbCr 4:2:0, the standard Huffman tables) for the status screens.
 * They are mostly flat colour, so uniform 16x16 areas skip the colour conversion and the DCT.
 */
#include "jpeg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *p; size_t n, cap; uint32_t acc; int bits; } W;

static void put(W *w, uint8_t b) {
    if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 65536; w->p = realloc(w->p, w->cap); }
    w->p[w->n++] = b;
}
static void put16(W *w, unsigned v) { put(w, v >> 8); put(w, v & 255); }
static void bits(W *w, unsigned code, int len) {
    w->acc = (w->acc << len) | (code & ((1u << len) - 1)); w->bits += len;
    while (w->bits >= 8) {
        uint8_t b = (w->acc >> (w->bits - 8)) & 255;
        put(w, b);
        if (b == 0xFF) put(w, 0);
        w->bits -= 8;
    }
}

/* Annex K tables */
static const uint8_t std_q[2][64] = {
    {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
     14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
     49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99},
    {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
     47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99}};
static const uint8_t dc_bits[2][16] = {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
                                       {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}};
static const uint8_t dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t ac_bits[2][16] = {{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
                                       {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77}};
static const uint8_t ac_vals[2][162] = {
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
     0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
     0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
     0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
     0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
     0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
     0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
     0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
     0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
     0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
     0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
     0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
     0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
     0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
     0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
     0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
     0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};

typedef struct { uint16_t code[256]; uint8_t len[256]; } Huff;
static Huff hdc[2], hac[2];
static uint8_t zz[64];          /* zigzag position -> natural index */
static float cosk[8][8];        /* c(u)/2 * cos((2x+1)u pi/16) */
static int ready;

static void huff(Huff *h, const uint8_t *nb, const uint8_t *vals) {
    unsigned code = 0, k = 0;
    for (int l = 1; l <= 16; l++, code <<= 1)
        for (int i = 0; i < nb[l - 1]; i++, code++, k++) { h->code[vals[k]] = code; h->len[vals[k]] = l; }
}

static void init(void) {
    int k = 0;
    for (int s = 0; s < 15; s++) {
        int lo = s < 8 ? 0 : s - 7, hi = s < 8 ? s : 7;
        if (s & 1) for (int i = lo; i <= hi; i++) zz[k++] = i * 8 + (s - i);
        else for (int i = hi; i >= lo; i--) zz[k++] = i * 8 + (s - i);
    }
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++)
            cosk[u][x] = (u ? 0.5f : 0.5f / sqrtf(2.f)) * cosf((2 * x + 1) * u * (float)M_PI / 16);
    for (int t = 0; t < 2; t++) { huff(&hdc[t], dc_bits[t], dc_vals); huff(&hac[t], ac_bits[t], ac_vals[t]); }
    ready = 1;
}

static void vlc(int v, unsigned *code, int *len) {
    unsigned a = v < 0 ? -v : v;
    int n = 0;
    while (a >> n) n++;
    *len = n;
    *code = v < 0 ? (unsigned)(v - 1) & ((1u << n) - 1) : (unsigned)v;
}

/* one 8x8 block of level-shifted samples (NULL = uniform with value `flat`) */
static void block(W *w, const float *s, float flat, const float *qinv, int t, int *dc) {
    int q[64];
    if (!s) {
        memset(q, 0, sizeof(q));
        q[0] = (int)lrintf(8 * flat * qinv[0]);
    } else {
        float tmp[64];
        for (int y = 0; y < 8; y++)
            for (int u = 0; u < 8; u++) {
                float a = 0;
                for (int x = 0; x < 8; x++) a += cosk[u][x] * s[y * 8 + x];
                tmp[y * 8 + u] = a;
            }
        for (int v = 0; v < 8; v++)
            for (int u = 0; u < 8; u++) {
                float a = 0;
                for (int y = 0; y < 8; y++) a += cosk[v][y] * tmp[y * 8 + u];
                q[v * 8 + u] = (int)lrintf(a * qinv[v * 8 + u]);
            }
    }
    unsigned code; int len;
    int d = q[0] - *dc; *dc = q[0];
    vlc(d, &code, &len);
    bits(w, hdc[t].code[len], hdc[t].len[len]);
    if (len) bits(w, code, len);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = q[zz[k]];
        if (!v) { run++; continue; }
        while (run > 15) { bits(w, hac[t].code[0xF0], hac[t].len[0xF0]); run -= 16; }
        vlc(v, &code, &len);
        int sym = run << 4 | len;
        bits(w, hac[t].code[sym], hac[t].len[sym]);
        bits(w, code, len);
        run = 0;
    }
    if (run) bits(w, hac[t].code[0], hac[t].len[0]);
}

size_t jpeg_encode_xrgb(const uint32_t *px, int width, int height, int stride, int quality, uint8_t **out) {
    if (!ready) init();
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    uint8_t qt[2][64]; float qinv[2][64];
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 64; i++) {
            int v = (std_q[t][i] * scale + 50) / 100;
            qt[t][i] = v < 1 ? 1 : v > 255 ? 255 : v;
            qinv[t][i] = 1.f / qt[t][i];
        }
    W w = {0};
    put16(&w, 0xFFD8);
    put16(&w, 0xFFDB); put16(&w, 132);
    for (int t = 0; t < 2; t++) { put(&w, t); for (int k = 0; k < 64; k++) put(&w, qt[t][zz[k]]); }
    put16(&w, 0xFFC0); put16(&w, 17); put(&w, 8); put16(&w, height); put16(&w, width); put(&w, 3);
    put(&w, 1); put(&w, 0x22); put(&w, 0);
    put(&w, 2); put(&w, 0x11); put(&w, 1);
    put(&w, 3); put(&w, 0x11); put(&w, 1);
    for (int t = 0; t < 2; t++) {
        int nac = 0; for (int i = 0; i < 16; i++) nac += ac_bits[t][i];
        put16(&w, 0xFFC4); put16(&w, 2 + 17 + 12 + 17 + nac);
        put(&w, t); for (int i = 0; i < 16; i++) put(&w, dc_bits[t][i]);
        for (int i = 0; i < 12; i++) put(&w, dc_vals[i]);
        put(&w, 0x10 | t); for (int i = 0; i < 16; i++) put(&w, ac_bits[t][i]);
        for (int i = 0; i < nac; i++) put(&w, ac_vals[t][i]);
    }
    put16(&w, 0xFFDA); put16(&w, 12); put(&w, 3);
    put(&w, 1); put(&w, 0x00); put(&w, 2); put(&w, 0x11); put(&w, 3); put(&w, 0x11);
    put(&w, 0); put(&w, 63); put(&w, 0);

    int dc[3] = {0, 0, 0};
    float Y[4][64], Cb[64], Cr[64];
    for (int my = 0; my < height; my += 16)
        for (int mx = 0; mx < width; mx += 16) {
            uint32_t first = px[(size_t)my * stride + mx];
            int flat = 1;
            for (int y = 0; y < 16 && flat; y++) {
                const uint32_t *row = px + (size_t)(my + y < height ? my + y : height - 1) * stride;
                for (int x = 0; x < 16; x++)
                    if ((row[mx + x < width ? mx + x : width - 1] & 0xFFFFFF) != (first & 0xFFFFFF)) { flat = 0; break; }
            }
            float r = first >> 16 & 255, g = first >> 8 & 255, b = first & 255;
            if (flat) {
                float fy = 0.299f * r + 0.587f * g + 0.114f * b - 128;
                for (int k = 0; k < 4; k++) block(&w, NULL, fy, qinv[0], 0, &dc[0]);
                block(&w, NULL, -0.168736f * r - 0.331264f * g + 0.5f * b, qinv[1], 1, &dc[1]);
                block(&w, NULL, 0.5f * r - 0.418688f * g - 0.081312f * b, qinv[1], 1, &dc[2]);
                continue;
            }
            memset(Cb, 0, sizeof(Cb)); memset(Cr, 0, sizeof(Cr));
            for (int y = 0; y < 16; y++) {
                const uint32_t *row = px + (size_t)(my + y < height ? my + y : height - 1) * stride;
                for (int x = 0; x < 16; x++) {
                    uint32_t p = row[mx + x < width ? mx + x : width - 1];
                    r = p >> 16 & 255; g = p >> 8 & 255; b = p & 255;
                    Y[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)] = 0.299f * r + 0.587f * g + 0.114f * b - 128;
                    int c = (y >> 1) * 8 + (x >> 1);
                    Cb[c] += 0.25f * (-0.168736f * r - 0.331264f * g + 0.5f * b);
                    Cr[c] += 0.25f * (0.5f * r - 0.418688f * g - 0.081312f * b);
                }
            }
            for (int k = 0; k < 4; k++) block(&w, Y[k], 0, qinv[0], 0, &dc[0]);
            block(&w, Cb, 0, qinv[1], 1, &dc[1]);
            block(&w, Cr, 0, qinv[1], 1, &dc[2]);
        }
    bits(&w, 0x7F, 7);   /* pad the last byte with ones */
    put16(&w, 0xFFD9);
    *out = w.p;
    return w.n;
}
