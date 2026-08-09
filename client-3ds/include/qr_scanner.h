#ifndef THREEGENT_QR_SCANNER_H
#define THREEGENT_QR_SCANNER_H

#include <3ds.h>

#include <stdbool.h>
#include <stddef.h>

/*
 * QR decoding for the pairing viewfinder, backed by the vendored quirc build in
 * `client-3ds/third_party/quirc` (D-021, R-006).
 *
 * Decoding a 400x240 frame costs far more than one frame's budget on Old 3DS,
 * so it runs on a lower-priority worker thread: the interactive loop keeps
 * drawing the viewfinder and pumping the network while a frame is analysed.
 * Submitting a frame while the worker is busy is a no-op, which bounds the work
 * to one frame in flight and needs no queue.
 */

#define QR_SCANNER_PAYLOAD_CAPACITY 256

bool qr_scanner_begin(char *error, size_t error_capacity);

/*
 * Hands an RGB565 frame to the worker if it is idle. Returns true when the
 * frame was accepted, false when a decode was already running.
 */
bool qr_scanner_submit(const u8 *rgb565, unsigned int width, unsigned int height);

bool qr_scanner_is_busy(void);

/* Number of frames analysed since qr_scanner_begin, for on-screen feedback. */
unsigned int qr_scanner_frames_examined(void);

/*
 * Returns a decoded payload once and clears it. The pointer stays valid until
 * the next call to this function or qr_scanner_end.
 */
const char *qr_scanner_take_payload(void);

void qr_scanner_end(void);

#endif
