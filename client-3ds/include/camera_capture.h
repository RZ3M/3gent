#ifndef THREEGENT_CAMERA_CAPTURE_H
#define THREEGENT_CAMERA_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <3ds.h>

#define THREEGENT_PHOTO_WIDTH 400
#define THREEGENT_PHOTO_HEIGHT 240
#define THREEGENT_PHOTO_BYTES \
    (THREEGENT_PHOTO_WIDTH * THREEGENT_PHOTO_HEIGHT * 2)

bool camera_capture_initialize(char *error, size_t error_capacity);
bool camera_capture_photo(
    u8 *rgb565,
    size_t capacity,
    char *error,
    size_t error_capacity
);
void camera_capture_draw_preview(const u8 *rgb565);
void camera_capture_shutdown(void);

#endif
