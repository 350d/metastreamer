/* telem.h — telemetry from goggles DUML traffic (battery, link, devices, camera, recording) + message monitor. */
#ifndef MS_TELEM_H
#define MS_TELEM_H
#include <stddef.h>
#include <stdint.h>

/* every valid DUML frame from the goggles */
void telem_on_duml(uint8_t src, uint8_t dst, uint8_t flags, uint8_t cmd_set, uint8_t cmd_id,
                   const uint8_t *payload, size_t len);
/* JSON object with parsed telemetry (a complete object {...}, including the outer braces) */
size_t telem_json(char *out, size_t cap);
/* JSON array of all message types seen: for research */
size_t telem_duml_json(char *out, size_t cap);
/* -1 if unknown / stale */
int telem_battery(void);
int telem_link_quality(void);
int telem_recording(void);
/* raw frame ring for /api/duml/send: every frame gets a sequence number */
unsigned telem_raw_seq(void);
/* JSON array of frames with seq > since, from src (0 = any), as {"src","dst","resp","cmd","payload"} */
size_t telem_raw_json(unsigned since, uint8_t src, char *out, size_t cap);
/* 1 = the camera does not push its state, values must be polled */
int telem_camera_needs_poll(void);
/* camera frame-rate wire codes <-> fps (30=3, 48=15, 50=5, 60=6, 100=10, 120=7); -1 if unknown */
int telem_fps_hz(int wire);
int telem_fps_wire(int hz);
#endif
