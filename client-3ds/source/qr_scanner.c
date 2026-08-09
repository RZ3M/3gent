#include "qr_scanner.h"

#include "camera_capture.h"
#include "quirc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QR_WORKER_STACK_BYTES (48 * 1024)

static struct quirc *decoder;
static struct quirc_code *scanned_code;
static struct quirc_data *scanned_data;
static Thread worker;
static LightEvent frame_submitted;
static char payload[QR_SCANNER_PAYLOAD_CAPACITY];

/*
 * Flags shared with the worker. The worker is created on the default core, so
 * both threads run on the same CPU and the scheduler provides the ordering;
 * with the event handing the frame over, single-writer volatile flags are
 * enough. `busy` is set by the main thread and cleared by the worker,
 * `payload_ready` is set by the worker and cleared by the main thread, and
 * `stopping` is written once before the join.
 */
static volatile bool worker_busy;
static volatile bool worker_stopping;
static volatile bool payload_ready;
static volatile unsigned int frames_examined;

/* ITU-R BT.601 luma, kept in integers and folded to 5/6-bit channel ranges. */
static void convert_rgb565_to_gray(
    const u8 *rgb565,
    unsigned int width,
    unsigned int height,
    u8 *gray
)
{
    const u16 *source = (const u16 *)(const void *)rgb565;
    const size_t pixels = (size_t)width * (size_t)height;
    for (size_t index = 0; index < pixels; index++) {
        const u16 pixel = source[index];
        const unsigned int red = ((pixel >> 11) & 0x1Fu) << 3;
        const unsigned int green = ((pixel >> 5) & 0x3Fu) << 2;
        const unsigned int blue = (pixel & 0x1Fu) << 3;
        gray[index] = (u8)((red * 77u + green * 151u + blue * 28u) >> 8);
    }
}

static void qr_worker(void *argument)
{
    (void)argument;
    while (true) {
        LightEvent_Wait(&frame_submitted);
        if (worker_stopping) {
            return;
        }
        if (!worker_busy) {
            continue;
        }

        quirc_end(decoder);
        const int count = quirc_count(decoder);
        for (int index = 0; index < count && !payload_ready; index++) {
            quirc_extract(decoder, index, scanned_code);
            if (quirc_decode(scanned_code, scanned_data) != QUIRC_SUCCESS) {
                /* ISO 18004:2015 allows a mirrored symbol; try the flip. */
                quirc_flip(scanned_code);
                if (quirc_decode(scanned_code, scanned_data) != QUIRC_SUCCESS) {
                    continue;
                }
            }
            int length = scanned_data->payload_len;
            if (length <= 0) {
                continue;
            }
            if (length > (int)sizeof(payload) - 1) {
                length = (int)sizeof(payload) - 1;
            }
            memcpy(payload, scanned_data->payload, (size_t)length);
            payload[length] = '\0';
            payload_ready = true;
        }

        frames_examined++;
        worker_busy = false;
    }
}

bool qr_scanner_begin(char *error, size_t error_capacity)
{
    if (decoder != NULL) {
        return true;
    }

    decoder = quirc_new();
    scanned_code = malloc(sizeof(*scanned_code));
    scanned_data = malloc(sizeof(*scanned_data));
    if (decoder == NULL || scanned_code == NULL || scanned_data == NULL
        || quirc_resize(decoder, THREEGENT_PHOTO_WIDTH, THREEGENT_PHOTO_HEIGHT) < 0) {
        snprintf(error, error_capacity, "not enough memory for the QR decoder");
        qr_scanner_end();
        return false;
    }

    payload[0] = '\0';
    payload_ready = false;
    worker_busy = false;
    worker_stopping = false;
    frames_examined = 0;
    LightEvent_Init(&frame_submitted, RESET_ONESHOT);

    /*
     * One priority step below this thread. Decoding a frame costs far more than
     * a frame's budget, so the interactive loop has to be able to preempt it.
     */
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    if (priority >= 0x3F) {
        priority = 0x3E;
    }
    worker = threadCreate(
        qr_worker,
        NULL,
        QR_WORKER_STACK_BYTES,
        priority + 1,
        -2,
        false
    );
    if (worker == NULL) {
        snprintf(error, error_capacity, "could not start the QR decoder thread");
        qr_scanner_end();
        return false;
    }
    return true;
}

bool qr_scanner_submit(const u8 *rgb565, unsigned int width, unsigned int height)
{
    if (decoder == NULL || worker_busy || payload_ready || rgb565 == NULL) {
        return false;
    }

    int image_width = 0;
    int image_height = 0;
    u8 *image = quirc_begin(decoder, &image_width, &image_height);
    if (image == NULL
        || (unsigned int)image_width != width
        || (unsigned int)image_height != height) {
        return false;
    }

    convert_rgb565_to_gray(rgb565, width, height, image);
    worker_busy = true;
    LightEvent_Signal(&frame_submitted);
    return true;
}

bool qr_scanner_is_busy(void)
{
    return worker_busy;
}

unsigned int qr_scanner_frames_examined(void)
{
    return frames_examined;
}

const char *qr_scanner_take_payload(void)
{
    if (!payload_ready) {
        return NULL;
    }
    payload_ready = false;
    return payload;
}

void qr_scanner_end(void)
{
    if (worker != NULL) {
        worker_stopping = true;
        LightEvent_Signal(&frame_submitted);
        threadJoin(worker, U64_MAX);
        threadFree(worker);
        worker = NULL;
    }
    if (decoder != NULL) {
        quirc_destroy(decoder);
        decoder = NULL;
    }
    free(scanned_code);
    scanned_code = NULL;
    free(scanned_data);
    scanned_data = NULL;
    worker_busy = false;
    payload_ready = false;
}
