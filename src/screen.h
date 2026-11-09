/* screen.h — the status screen shown until there is a picture from the goggles: on HDMI and as /snapshot. */
#ifndef MS_SCREEN_H
#define MS_SCREEN_H
#include <stddef.h>
#include <stdint.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

/* how far the bridge got: each stage needs the ones before it (on screen, the boot comes first: SYSTEM) */
enum { STAGE_USB, STAGE_GOGGLES, STAGE_AIR_UNIT, STAGE_VIDEO };
enum { WIFI_OFF, WIFI_SEARCHING, WIFI_CONNECTED, WIFI_ERROR };

typedef struct {
    int stage;          /* the step being waited for */
    int reconnecting;   /* the goggles went silent: the USB session is being restarted */
    int wifi;           /* WIFI_* */
    int wifi_level;     /* 0..3 */
    char ssid[33], ip[16];
    char wifi_error[64];   /* WIFI_ERROR: why (from metastreamer-wifi) */
    int undervoltage;   /* 0 never, 1 earlier, 2 now */
    int starting;       /* the system is still booting (the splash): title STARTING */
    char boot_step[40]; /* ... and what it is doing */
    int boot_done, boot_total;   /* init scripts finished / all of them */
} ScreenState;

/* Draw the screen if the state changed and hand it to HDMI */
void screen_update(const ScreenState *s);
/* Animations: the loader of the step being waited for, the Wi-Fi bars while connecting */
enum { ANIM_LOADER, ANIM_WIFI, SCREEN_ANIMS };
/* 1 if animation a is on the screen: its frame count and ms per frame */
int screen_anim_info(int a, int *n, int *ms);
/* Draw frame k of animation a into the screen; 1 and its rectangle, or 0 if it is not on the screen */
int screen_anim_frame(int a, int k, int *x, int *y, int *w, int *h);
/* The current screen as a JPEG (encoded when first asked for after a change) */
const uint8_t *screen_jpeg(size_t *n);
/* The current screen, SCREEN_W x SCREEN_H XRGB8888 */
const uint32_t *screen_pixels(void);
/* Per row: the columns [x0, x1) that may not be black (x0 >= x1: the whole row is black) */
void screen_extents(const uint16_t **x0, const uint16_t **x1);
#endif
