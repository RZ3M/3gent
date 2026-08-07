#include "microphone.h"

#include <3ds.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIC_SHARED_BUFFER_ALIGNMENT 0x1000
#define MIC_SHARED_BUFFER_SIZE 0x30000
#define PCM_BYTES_PER_SAMPLE (THREEGENT_MIC_BITS_PER_SAMPLE / 8)
#define PCM_BYTES_PER_SECOND \
    (THREEGENT_MIC_SAMPLE_RATE_HZ * THREEGENT_MIC_CHANNELS \
     * PCM_BYTES_PER_SAMPLE)
#define PCM_CAPACITY (PCM_BYTES_PER_SECOND * THREEGENT_MIC_MAX_SECONDS)
#define ARM11_DATA_CACHE_LINE_SIZE 32U

static u8 *shared_buffer;
static u32 shared_data_size;
static u32 shared_read_offset;
static u32 stopped_write_offset;
static u32 last_write_offset;
static size_t total_pcm_size;
static u64 capture_started_ms;
static u64 capture_finished_ms;
static u64 last_offset_change_ms;
static unsigned int offset_change_count;
static unsigned int level_percent;
static bool mic_ready;
static bool sampling;
static bool service_sampling;
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

static bool invalidate_shared_range(
    const void *address,
    size_t size,
    char *error,
    size_t error_capacity
)
{
    if (size == 0) {
        return true;
    }

    uintptr_t start = (uintptr_t)address;
    uintptr_t aligned_start = start & ~(ARM11_DATA_CACHE_LINE_SIZE - 1U);
    uintptr_t end = start + size;
    uintptr_t aligned_end = (
        end + ARM11_DATA_CACHE_LINE_SIZE - 1U
    ) & ~(ARM11_DATA_CACHE_LINE_SIZE - 1U);

    Result result = svcInvalidateProcessDataCache(
        CUR_PROCESS_HANDLE,
        (u32)aligned_start,
        (u32)(aligned_end - aligned_start)
    );
    if (R_FAILED(result)) {
        set_result_error(error, error_capacity, "mic cache invalidate", result);
        return false;
    }
    return true;
}

static bool read_write_offset(
    u32 *write_offset,
    char *error,
    size_t error_capacity
)
{
    if (!invalidate_shared_range(
            shared_buffer + shared_data_size,
            sizeof(u32),
            error,
            error_capacity
        )) {
        return false;
    }

    *write_offset = micGetLastSampleOffset();
    if (*write_offset >= shared_data_size) {
        set_error(error, error_capacity, "microphone write offset is invalid");
        return false;
    }
    return true;
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

    bool active = false;
    result = MICU_IsSampling(&active);
    if (R_FAILED(result)) {
        MICU_StopSampling();
        set_result_error(error, error_capacity, "MICU_IsSampling", result);
        return false;
    }
    if (!active) {
        set_error(error, error_capacity, "MIC service stopped immediately");
        return false;
    }

    total_pcm_size = 0;
    level_percent = 0;
    capture_full = false;
    stopped_write_offset = 0;
    last_write_offset = 0;
    offset_change_count = 0;
    capture_started_ms = osGetTime();
    capture_finished_ms = 0;
    last_offset_change_ms = capture_started_ms;
    capture_open = true;
    sampling = true;
    service_sampling = true;
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
    u32 write_offset = stopped_write_offset;
    if (sampling) {
        bool active = false;
        Result result = MICU_IsSampling(&active);
        if (R_FAILED(result)) {
            set_result_error(error, error_capacity, "MICU_IsSampling", result);
            return false;
        }
        service_sampling = active;
        if (!active) {
            sampling = false;
            capture_open = false;
            set_error(error, error_capacity, "MIC service stopped while R was held");
            return false;
        }
        if (!read_write_offset(
                &write_offset,
                error,
                error_capacity
            )) {
            return false;
        }
    }

    const u64 now_ms = osGetTime();
    if (write_offset != last_write_offset) {
        last_write_offset = write_offset;
        last_offset_change_ms = now_ms;
        offset_change_count++;
    }

    u32 read_offset = shared_read_offset;
    const size_t remaining_capture_capacity = PCM_CAPACITY - total_pcm_size;
    size_t copy_limit = destination_capacity;
    if (copy_limit > remaining_capture_capacity) {
        copy_limit = remaining_capture_capacity;
    }

    size_t available = write_offset >= read_offset
        ? write_offset - read_offset
        : shared_data_size - read_offset + write_offset;
    if (copy_limit > available) {
        copy_limit = available;
    }

    size_t first_copy_size = copy_limit;
    size_t bytes_until_wrap = shared_data_size - read_offset;
    if (first_copy_size > bytes_until_wrap) {
        first_copy_size = bytes_until_wrap;
    }
    if (!invalidate_shared_range(
            shared_buffer + read_offset,
            first_copy_size,
            error,
            error_capacity
        )) {
        return false;
    }
    memcpy(output, shared_buffer + read_offset, first_copy_size);

    size_t second_copy_size = copy_limit - first_copy_size;
    if (!invalidate_shared_range(
            shared_buffer,
            second_copy_size,
            error,
            error_capacity
        )) {
        return false;
    }
    memcpy(output + first_copy_size, shared_buffer, second_copy_size);

    *bytes_read = copy_limit;
    read_offset = (read_offset + copy_limit) % shared_data_size;
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
        service_sampling = false;
        capture_finished_ms = now_ms;
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
        service_sampling = false;
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
        if (!read_write_offset(
                &stopped_write_offset,
                error,
                error_capacity
            )) {
            capture_open = false;
            return false;
        }
        if (stopped_write_offset != last_write_offset) {
            last_write_offset = stopped_write_offset;
            last_offset_change_ms = osGetTime();
            offset_change_count++;
        }
    }

    capture_finished_ms = osGetTime();
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

unsigned int microphone_wall_duration_ms(void)
{
    if (capture_started_ms == 0) {
        return 0;
    }
    u64 end_ms = capture_finished_ms != 0
        ? capture_finished_ms
        : osGetTime();
    return (unsigned int)(end_ms - capture_started_ms);
}

unsigned int microphone_level_percent(void)
{
    return level_percent;
}

size_t microphone_total_pcm_size(void)
{
    return total_pcm_size;
}

unsigned int microphone_last_write_offset(void)
{
    return last_write_offset;
}

unsigned int microphone_offset_change_count(void)
{
    return offset_change_count;
}

unsigned int microphone_stall_ms(void)
{
    if (last_offset_change_ms == 0 || !sampling) {
        return 0;
    }
    return (unsigned int)(osGetTime() - last_offset_change_ms);
}

bool microphone_service_is_sampling(void)
{
    return service_sampling;
}
