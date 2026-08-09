#include "camera_capture.h"

#include <stdio.h>
#include <string.h>

#define CAMERA_WAIT_TIMEOUT_NS 300000000ULL

static bool camera_ready;

static bool camera_result_ok(
    Result result,
    const char *operation,
    char *error,
    size_t error_capacity
)
{
    if (R_SUCCEEDED(result)) {
        return true;
    }
    snprintf(error, error_capacity, "%s failed (0x%08lx)", operation, (unsigned long)result);
    return false;
}

bool camera_capture_initialize(char *error, size_t error_capacity)
{
    if (camera_ready) {
        return true;
    }
    if (!camera_result_ok(camInit(), "camInit", error, error_capacity)) {
        return false;
    }
    camera_ready = true;
    if (!camera_result_ok(
            CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A),
            "camera size",
            error,
            error_capacity
        ) || !camera_result_ok(
            CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A),
            "camera format",
            error,
            error_capacity
        ) || !camera_result_ok(
            CAMU_SetNoiseFilter(SELECT_OUT1, true),
            "camera noise filter",
            error,
            error_capacity
        ) || !camera_result_ok(
            CAMU_SetAutoExposure(SELECT_OUT1, true),
            "camera exposure",
            error,
            error_capacity
        ) || !camera_result_ok(
            CAMU_SetAutoWhiteBalance(SELECT_OUT1, true),
            "camera white balance",
            error,
            error_capacity
        ) || !camera_result_ok(
            CAMU_SetTrimming(PORT_CAM1, false),
            "camera trimming",
            error,
            error_capacity
        )) {
        camera_capture_shutdown();
        return false;
    }
    return true;
}

bool camera_capture_photo(
    u8 *rgb565,
    size_t capacity,
    char *error,
    size_t error_capacity
)
{
    if (!camera_ready || rgb565 == NULL || capacity < THREEGENT_PHOTO_BYTES) {
        snprintf(error, error_capacity, "camera buffer is unavailable");
        return false;
    }
    u32 transfer_bytes = 0;
    Handle receive_event = 0;
    bool active = false;
    bool capturing = false;
    Result result = CAMU_GetMaxBytes(
        &transfer_bytes,
        THREEGENT_PHOTO_WIDTH,
        THREEGENT_PHOTO_HEIGHT
    );
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetTransferBytes(
            PORT_CAM1,
            transfer_bytes,
            THREEGENT_PHOTO_WIDTH,
            THREEGENT_PHOTO_HEIGHT
        );
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_Activate(SELECT_OUT1);
        active = R_SUCCEEDED(result);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_ClearBuffer(PORT_CAM1);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_StartCapture(PORT_CAM1);
        capturing = R_SUCCEEDED(result);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetReceiving(
            &receive_event,
            rgb565,
            PORT_CAM1,
            THREEGENT_PHOTO_BYTES,
            (s16)transfer_bytes
        );
    }
    if (R_SUCCEEDED(result)) {
        result = svcWaitSynchronization(receive_event, CAMERA_WAIT_TIMEOUT_NS);
    }
    if (R_SUCCEEDED(result)) {
        CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL);
    }
    if (capturing) {
        CAMU_StopCapture(PORT_CAM1);
    }
    if (receive_event != 0) {
        svcCloseHandle(receive_event);
    }
    if (active) {
        CAMU_Activate(SELECT_NONE);
    }
    if (R_FAILED(result)) {
        snprintf(error, error_capacity, "camera capture failed (0x%08lx)", (unsigned long)result);
        return false;
    }
    return true;
}

/* ------------------------------------------------------ streaming preview -- */

static bool stream_active;
static bool stream_capturing;
static Handle stream_event;
static Handle stream_error_event;
static u8 *stream_buffer;
static s16 stream_transfer_bytes;
static u64 stream_last_frame_ms;
static unsigned int stream_recoveries;

/*
 * The camera raises a buffer error and stops feeding the armed transfer. It is
 * not rare and it is not fatal, but nothing recovers on its own: the receive
 * event simply never signals again and the viewfinder freezes wherever it was.
 * The official devkitPro camera example watches this same interrupt and
 * restarts capture, which is what the two constants below are for.
 */
#define CAMERA_STREAM_STALL_MS 1500ULL

static void stream_close_event(void)
{
    if (stream_event != 0) {
        svcCloseHandle(stream_event);
        stream_event = 0;
    }
}

/* Queues the next frame into the caller's buffer without waiting for it. */
static bool stream_arm(char *error, size_t error_capacity)
{
    stream_close_event();
    return camera_result_ok(
        CAMU_SetReceiving(
            &stream_event,
            stream_buffer,
            PORT_CAM1,
            THREEGENT_PHOTO_BYTES,
            stream_transfer_bytes
        ),
        "camera receive",
        error,
        error_capacity
    );
}

/* Stops, clears and restarts the capture, then re-arms a transfer. */
static bool stream_restart(char *error, size_t error_capacity)
{
    stream_close_event();
    if (stream_capturing) {
        CAMU_StopCapture(PORT_CAM1);
        stream_capturing = false;
    }
    if (!camera_result_ok(
            CAMU_ClearBuffer(PORT_CAM1),
            "camera clear",
            error,
            error_capacity
        )
        || !camera_result_ok(
            CAMU_StartCapture(PORT_CAM1),
            "camera restart",
            error,
            error_capacity
        )) {
        return false;
    }
    stream_capturing = true;
    if (!stream_arm(error, error_capacity)) {
        return false;
    }
    stream_recoveries++;
    stream_last_frame_ms = osGetTime();
    return true;
}

bool camera_capture_stream_begin(
    u8 *rgb565,
    size_t capacity,
    char *error,
    size_t error_capacity
)
{
    if (!camera_ready || rgb565 == NULL || capacity < THREEGENT_PHOTO_BYTES) {
        snprintf(error, error_capacity, "camera buffer is unavailable");
        return false;
    }
    if (stream_active) {
        return true;
    }

    u32 transfer_bytes = 0;
    stream_buffer = rgb565;
    if (!camera_result_ok(
            CAMU_GetMaxBytes(
                &transfer_bytes,
                THREEGENT_PHOTO_WIDTH,
                THREEGENT_PHOTO_HEIGHT
            ),
            "camera transfer size",
            error,
            error_capacity
        )
        || !camera_result_ok(
            CAMU_SetTransferBytes(
                PORT_CAM1,
                transfer_bytes,
                THREEGENT_PHOTO_WIDTH,
                THREEGENT_PHOTO_HEIGHT
            ),
            "camera transfer bytes",
            error,
            error_capacity
        )
        || !camera_result_ok(
            CAMU_Activate(SELECT_OUT1),
            "camera activate",
            error,
            error_capacity
        )) {
        camera_capture_stream_end();
        return false;
    }
    stream_active = true;
    stream_transfer_bytes = (s16)transfer_bytes;
    stream_recoveries = 0;

    /* Without this handle a buffer error is invisible and stalls the stream. */
    if (!camera_result_ok(
            CAMU_GetBufferErrorInterruptEvent(&stream_error_event, PORT_CAM1),
            "camera error event",
            error,
            error_capacity
        )) {
        camera_capture_stream_end();
        return false;
    }

    if (!camera_result_ok(
            CAMU_ClearBuffer(PORT_CAM1),
            "camera clear",
            error,
            error_capacity
        )
        || !camera_result_ok(
            CAMU_StartCapture(PORT_CAM1),
            "camera start",
            error,
            error_capacity
        )) {
        camera_capture_stream_end();
        return false;
    }
    stream_capturing = true;

    if (!stream_arm(error, error_capacity)) {
        camera_capture_stream_end();
        return false;
    }
    stream_last_frame_ms = osGetTime();
    return true;
}

bool camera_capture_stream_read(
    bool *frame_ready,
    char *error,
    size_t error_capacity
)
{
    *frame_ready = false;
    if (!stream_active || stream_event == 0) {
        snprintf(error, error_capacity, "camera stream is not running");
        return false;
    }

    /* A buffer error is reported instead of the frame, so check it first. */
    if (stream_error_event != 0
        && R_DESCRIPTION(svcWaitSynchronization(stream_error_event, 0))
            != RD_TIMEOUT) {
        return stream_restart(error, error_capacity);
    }

    /* A zero timeout is what keeps this off the frame loop's critical path. */
    const Result result = svcWaitSynchronization(stream_event, 0);
    if (R_DESCRIPTION(result) == RD_TIMEOUT) {
        /*
         * Not every stall announces itself. Treat a long silence as one rather
         * than leaving the user looking at a frozen viewfinder with no way
         * back except cancelling.
         */
        if (osGetTime() - stream_last_frame_ms >= CAMERA_STREAM_STALL_MS) {
            return stream_restart(error, error_capacity);
        }
        return true;
    }
    if (R_FAILED(result)) {
        snprintf(
            error,
            error_capacity,
            "camera stream failed (0x%08lx)",
            (unsigned long)result
        );
        return false;
    }

    stream_last_frame_ms = osGetTime();
    *frame_ready = true;
    return true;
}

unsigned int camera_capture_stream_recoveries(void)
{
    return stream_recoveries;
}

bool camera_capture_stream_release(char *error, size_t error_capacity)
{
    if (!stream_active) {
        snprintf(error, error_capacity, "camera stream is not running");
        return false;
    }
    return stream_arm(error, error_capacity);
}

void camera_capture_stream_end(void)
{
    stream_close_event();
    if (stream_error_event != 0) {
        svcCloseHandle(stream_error_event);
        stream_error_event = 0;
    }
    if (stream_capturing) {
        CAMU_StopCapture(PORT_CAM1);
        stream_capturing = false;
    }
    if (stream_active) {
        CAMU_Activate(SELECT_NONE);
        stream_active = false;
    }
    stream_buffer = NULL;
}

void camera_capture_shutdown(void)
{
    camera_capture_stream_end();
    if (camera_ready) {
        camExit();
        camera_ready = false;
    }
}
