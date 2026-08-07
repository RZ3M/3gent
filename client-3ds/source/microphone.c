#include "microphone.h"

#include <3ds.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#define MIC_SHARED_BUFFER_ALIGNMENT 0x1000
#define MIC_SHARED_BUFFER_SIZE 0x30000
#define PCM_BYTES_PER_SAMPLE (THREEGENT_MIC_BITS_PER_SAMPLE / 8)
#define PCM_BYTES_PER_SECOND \
    (THREEGENT_MIC_SAMPLE_RATE_HZ * THREEGENT_MIC_CHANNELS \
     * PCM_BYTES_PER_SAMPLE)
#define PCM_CAPACITY (PCM_BYTES_PER_SECOND * THREEGENT_MIC_MAX_SECONDS)

static u8 *shared_buffer;
static u32 shared_data_size;
static u32 shared_read_offset;
static u32 stopped_write_offset;
static size_t total_pcm_size;
static unsigned int level_percent;
static bool mic_ready;
static bool sampling;
static bool capture_open;
static bool capture_full;

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error == NULL || capacity == 0) {
        return;
    }
    snprintf(error, capacity, "%s", message);
}

static void set_result_error(
    char *error,
    size_t capacity,
    const char *operation,
    Result result
)
{
    if (error == NULL || capacity == 0) {
        return;
    }
    snprintf(
        error,
        capacity,
        "%s failed (0x%08lx)",
        operation,
        (unsigned long)result
    );
}

bool microphone_initialize(char *error, size_t error_capacity)
{
    if (mic_ready) {
        return true;
    }

    shared_buffer = (u8 *)memalign(
        MIC_SHARED_BUFFER_ALIGNMENT,
        MIC_SHARED_BUFFER_SIZE
    );
    if (shared_buffer == NULL) {
        set_error(error, error_capacity, "microphone buffer allocation failed");
        return false;
    }

    Result result = micInit(shared_buffer, MIC_SHARED_BUFFER_SIZE);
    if (R_FAILED(result)) {
        set_result_error(error, error_capacity, "micInit", result);
        free(shared_buffer);
        shared_buffer = NULL;
        return false;
    }

    shared_data_size = micGetSampleDataSize();
    if (shared_data_size == 0) {
        set_error(error, error_capacity, "microphone sample buffer is empty");
        micExit();
        free(shared_buffer);
        shared_buffer = NULL;
        return false;
    }

    mic_ready = true;
    return true;
}

void microphone_shutdown(void)
{
    if (sampling) {
        MICU_StopSampling();
        sampling = false;
    }
    capture_open = false;
    if (mic_ready) {
        micExit();
        mic_ready = false;
    }

    free(shared_buffer);
    shared_buffer = NULL;
    shared_data_size = 0;
}

bool microphone_begin_capture(char *error, size_t error_capacity)
{
    if (!mic_ready) {
        set_error(error, error_capacity, "microphone service is unavailable");
        return false;
    }
    if (sampling) {
        set_error(error, error_capacity, "microphone is already recording");
        return false;
    }

    shared_read_offset = 0;

    Result result = MICU_StartSampling(
        MICU_ENCODING_PCM16_SIGNED,
        MICU_SAMPLE_RATE_16360,
        0,
        shared_data_size,
        true
    );
    if (R_FAILED(result)) {
        set_result_error(error, error_capacity, "MICU_StartSampling", result);
        return false;
    }

    total_pcm_size = 0;
    level_percent = 0;
    capture_full = false;
    stopped_write_offset = 0;
    capture_open = true;
    sampling = true;
    return true;
}

bool microphone_read_capture(
    void *destination,
    size_t destination_capacity,
    size_t *bytes_read,
    char *error,
    size_t error_capacity
)
{
    if (bytes_read == NULL) {
        set_error(error, error_capacity, "microphone byte count is unavailable");
        return false;
    }
    *bytes_read = 0;

    if (!capture_open) {
        set_error(error, error_capacity, "microphone capture is not active");
        return false;
    }
    if (destination == NULL || destination_capacity == 0) {
        set_error(error, error_capacity, "microphone destination is unavailable");
        return false;
    }
    if (capture_full) {
        return true;
    }

    u8 *output = (u8 *)destination;
    const u32 write_offset = sampling
        ? micGetLastSampleOffset()
        : stopped_write_offset;
    u32 read_offset = shared_read_offset;
    const size_t remaining_capture_capacity = PCM_CAPACITY - total_pcm_size;
    size_t copy_limit = destination_capacity;
    if (copy_limit > remaining_capture_capacity) {
        copy_limit = remaining_capture_capacity;
    }

    while (read_offset != write_offset && *bytes_read < copy_limit) {
        output[*bytes_read] = shared_buffer[read_offset];
        (*bytes_read)++;
        read_offset = (read_offset + 1) % shared_data_size;
    }
    shared_read_offset = read_offset;
    total_pcm_size += *bytes_read;

    if (*bytes_read > 0) {
        unsigned int peak = 0;
        size_t sample_offset = 0;
        while (sample_offset + 1 < *bytes_read) {
            const u8 *sample_bytes = output + sample_offset;
            int sample = (int)(s16)(
                (u16)sample_bytes[0] | ((u16)sample_bytes[1] << 8)
            );
            unsigned int magnitude = sample < 0
                ? (unsigned int)(-sample)
                : (unsigned int)sample;
            if (magnitude > peak) {
                peak = magnitude;
            }
            sample_offset += PCM_BYTES_PER_SAMPLE;
        }
        level_percent = (peak * 100U) / 32768U;
    }

    if (total_pcm_size == PCM_CAPACITY) {
        Result result = MICU_StopSampling();
        sampling = false;
        capture_full = true;
        stopped_write_offset = shared_read_offset;
        if (R_FAILED(result)) {
            set_result_error(error, error_capacity, "MICU_StopSampling", result);
            return false;
        }
    }

    return true;
}

bool microphone_finish_capture(char *error, size_t error_capacity)
{
    if (!capture_open) {
        set_error(error, error_capacity, "microphone capture is not active");
        return false;
    }

    if (sampling) {
        Result result = MICU_StopSampling();
        sampling = false;
        if (R_FAILED(result)) {
            set_result_error(
                error,
                error_capacity,
                "MICU_StopSampling",
                result
            );
            capture_open = false;
            return false;
        }
        stopped_write_offset = micGetLastSampleOffset();
    }

    level_percent = 0;
    return true;
}

bool microphone_is_ready(void)
{
    return mic_ready;
}

bool microphone_is_sampling(void)
{
    return sampling;
}

bool microphone_capture_is_full(void)
{
    return capture_full;
}

unsigned int microphone_duration_ms(void)
{
    return (unsigned int)(
        ((u64)total_pcm_size * 1000U) / PCM_BYTES_PER_SECOND
    );
}

unsigned int microphone_level_percent(void)
{
    return level_percent;
}

size_t microphone_total_pcm_size(void)
{
    return total_pcm_size;
}
