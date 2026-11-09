/* jpeg.h — software baseline JPEG encoder for the status screens. */
#ifndef MS_JPEG_H
#define MS_JPEG_H
#include <stddef.h>
#include <stdint.h>

/* XRGB8888 pixels (stride in pixels) -> JPEG in a malloc'd buffer (*out, caller frees); returns its size */
size_t jpeg_encode_xrgb(const uint32_t *px, int width, int height, int stride, int quality, uint8_t **out);
#endif
