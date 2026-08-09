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

/*
 * Continuous capture for the pairing viewfinder. Unlike `camera_capture_photo`
 * this never blocks for a frame: `camera_capture_stream_read` reports whether
 * one has arrived since the last call, so the frame loop keeps drawing and
 * pumping the network while the user aims at the QR code.
 *
 * Frames are written into the caller's buffer, which must hold
 * THREEGENT_PHOTO_BYTES and must stay alive until the stream ends.
 *
 * Delivery and re-arming are separate on purpose. The camera only writes to the
 * buffer while a transfer is armed, so a frame reported by
 * `camera_capture_stream_read` is stable until `camera_capture_stream_release`
 * asks for the next one. Read, use the pixels, then release.
 */
bool camera_capture_stream_begin(
    u8 *rgb565,
    size_t capacity,
    char *error,
    size_t error_capacity
);
bool camera_capture_stream_read(bool *frame_ready, char *error, size_t error_capacity);
bool camera_capture_stream_release(char *error, size_t error_capacity);

/*
 * How many times the stream has restarted itself after a camera buffer error or
 * a stall. Recovery is silent and expected, so this exists to make it visible
 * during a hardware run rather than to be acted on.
 */
unsigned int camera_capture_stream_recoveries(void);

void camera_capture_stream_end(void);

void camera_capture_shutdown(void);

#endif
