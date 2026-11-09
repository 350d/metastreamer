/* hdmi.h — hardware H.264 decode (V4L2 M2M, bcm2835-codec), HDMI output (KMS) and JPEG snapshots. */
#ifndef MS_HDMI_H
#define MS_HDMI_H
#include <stddef.h>
#include <stdint.h>

/* Starts in its own thread. with_display=0: decode only (for /snapshot). */
int hdmi_start(int with_display);
/* Output mode before hdmi_start: "WxH[@HZ]" or "auto" (EDID preferred). Default 1920x1080, 60 Hz then 50. */
int hdmi_set_mode(const char *s);
/* Frame in Annex-B; keyframes are already prefixed with SPS/PPS. Call from the main thread. */
void hdmi_feed_au(const uint8_t *annexb, size_t n, int key);
int hdmi_decoder_running(void);
/* The picture under the video (XRGB8888, scaled to the display): visible whenever no video frame came for 1 s.
 * Call from the main thread; the pixels are copied. */
void hdmi_show_screen(const uint32_t *px, int w, int h);
/* Animation `slot` (0..3) on that picture, played in step with the display: n frames of fw x fh pixels one after
 * another, at x, y (screen pixels), frame_ms each; x < 0 = none. The frames must stay valid. */
void hdmi_set_animation(int slot, const uint32_t *frames, int n, int fw, int fh, int x, int y, int frame_ms);
void hdmi_stats(char *out, size_t cap);
size_t hdmi_json(char *out, size_t cap);
/* Snapshots: request, then wait for wake-up via wake_fd (byte 's' = ready, 'e' = error). */
void hdmi_snapshot_request(void);
int hdmi_snapshot_wake_fd(void);
const uint8_t *hdmi_snapshot_lock(size_t *n, unsigned *seq, double *age);
void hdmi_snapshot_unlock(void);
#endif
