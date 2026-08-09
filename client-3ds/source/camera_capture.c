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

void camera_capture_shutdown(void)
{
    if (camera_ready) {
        camExit();
        camera_ready = false;
    }
}
