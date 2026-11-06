/* usb.h — the AOA gadget on the goggles' USB port, served through FunctionFS. */
#ifndef MS_USB_H
#define MS_USB_H
#include <stddef.h>
#include <sys/types.h>

/* Build the gadget and put it on the bus; -1 on failure */
int usb_start(void);
/* Data from the goggles: poll this fd, then usb_read() (non-blocking) */
int usb_rx_fd(void);
ssize_t usb_read(void *b, size_t n);
/* Queue data for the goggles; dropped while they have not enabled us */
void usb_write(const void *b, size_t n);
/* Gadget events (enable, disable, control requests): poll this fd, then usb_events() */
int usb_event_fd(void);
void usb_events(void);
int usb_enabled(void);
/* Changes whenever the goggles enable or disable us: a new session starts from scratch */
unsigned usb_epoch(void);
/* Make the goggles open a new session (soft disconnect sequence, ~20 s); usb_tick() drives it */
void usb_restart_session(void);
int usb_restarting(void);
void usb_tick(void);
size_t usb_stats(char *out, size_t cap);
#endif
