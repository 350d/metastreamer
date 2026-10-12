/* hls.h — built-in H.264 -> fMP4 packager (per-frame stream + HLS) and HTTP server for metastreamer. */
#ifndef MS_HLS_H
#define MS_HLS_H
#include <poll.h>
#include <stddef.h>
#include <stdint.h>

void hls_start(int http_port);
/* Feed raw H.264 Annex-B (as received from the goggles). */
void hls_feed(const uint8_t *d, size_t n);
/* Sink for completed frames in Annex-B (keyframes are prefixed with SPS/PPS), e.g. HDMI. */
typedef void (*au_sink_fn)(const uint8_t *annexb, size_t n, int key);
void hls_set_au_sink(au_sink_fn f);
/* Append own descriptors to the poll array; return how many were added. */
int hls_pollfds(struct pollfd *p, int max);
/* Handle events on descriptors added by hls_pollfds. */
void hls_handle(const struct pollfd *p, int n);
void hls_stats(char *out, size_t cap);
/* call on every loop pass: /events, pending snapshots, stream rate */
void hls_tick(void);
/* JSON fragment about the goggles link for /status */
typedef size_t (*json_fn)(char *out, size_t cap);
void hls_set_link_fn(json_fn f);
/* camera settings from /api/set?key=value: 0 ok, -1 bad value, -2 bad key, -3 no link */
typedef int (*set_fn)(const char *key, const char *value);
void hls_set_camera_fn(set_fn f);
#endif
