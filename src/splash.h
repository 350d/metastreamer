/* splash.h — the boot screen on the console framebuffer, until metastreamer takes the display. */
#ifndef MS_SPLASH_H
#define MS_SPLASH_H
#define SPLASH_PID "/run/splash.pid"

/* metastreamer --splash: runs until SIGTERM (or a minute) */
int splash_main(void);
/* stop a running splash (pid in SPLASH_PID) */
void splash_stop(void);
#endif
