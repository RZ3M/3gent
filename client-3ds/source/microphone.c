#include "microphone.h"

#include <3ds.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIC_SHARED_BUFFER_ALIGNMENT 0x1000
#define MIC_SHARED_BUFFER_SIZE 0x30000
#define WAV_HEADER_SIZE 44
#define PCM_BYTES_PER_SAMPLE (THREEGENT_MIC_BITS_PER_SAMPLE / 8)
#define PCM_BYTES_PER_SECOND \
    (THREEGENT_MIC_SAMPLE_RATE_HZ * THREEGENT_MIC_CHANNELS \
     * PCM_BYTES_PER_SAMPLE)
#define PCM_CAPACITY (PCM_BYTES_PER_SECOND * THREEGENT_MIC_MAX_SECONDS)
#define WAV_CAPACITY (WAV_HEADER_SIZE + PCM_CAPACITY)

static u8 *shared_buffer;
static u32 shared_data_size;
static u32 shared_read_offset;
static u8 wav_buffer[WAV_CAPACITY];
static size_t pcm_size;
static unsigned int level_percent;
static bool mic_ready;
static bool sampling;
static bool capture_full;
static bool has_capture;

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

static void write_u16_le(u8 *destination, uint16_t value)
{
    destination[0] = (u8)(value & 0xff);
    destination[1] = (u8)((value >> 8) & 0xff);
}

static void write_u32_le(u8 *destination, uint32_t value)
{
    destination[0] = (u8)(value & 0xff);
    destination[1] = (u8)((value >> 8) & 0xff);
    destination[2] = (u8)((value >> 16) & 0xff);
    destination[3] = (u8)((value >> 24) & 0xff);
}

static void finalize_wav_header(void)
{
    const uint32_t data_size = (uint32_t)pcm_size;
    const uint32_t byte_rate = PCM_BYTES_PER_SECOND;
    const uint16_t block_align = THREEGENT_MIC_CHANNELS * PCM_BYTES_PER_SAMPLE;

    memcpy(wav_buffer, "RIFF", 4);
    write_u32_le(wav_buffer + 4, 36 + data_size);
    memcpy(wav_buffer + 8, "WAVE", 4);
    memcpy(wav_buffer + 12, "fmt ", 4);
    write_u32_le(wav_buffer + 16, 16);
    write_u16_le(wav_buffer + 20, 1);
    write_u16_le(wav_buffer + 22, THREEGENT_MIC_CHANNELS);
    write_u32_le(wav_buffer + 24, THREEGENT_MIC_SAMPLE_RATE_HZ);
    write_u32_le(wav_buffer + 28, byte_rate);
    write_u16_le(wav_buffer + 32, block_align);
    write_u16_le(wav_buffer + 34, THREEGENT_MIC_BITS_PER_SAMPLE);
    memcpy(wav_buffer + 36, "data", 4);
    write_u32_le(wav_buffer + 40, data_size);
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

    pcm_size = 0;
    level_percent = 0;
    capture_full = false;
    has_capture = false;
    memset(wav_buffer, 0, WAV_HEADER_SIZE);
    sampling = true;
    return true;
}

bool microphone_poll_capture(char *error, size_t error_capacity)
{
    if (!sampling) {
        return true;
    }

    const size_t original_pcm_size = pcm_size;
    const u32 write_offset = micGetLastSampleOffset();
    u32 read_offset = shared_read_offset;

    while (read_offset != write_offset && pcm_size < PCM_CAPACITY) {
        wav_buffer[WAV_HEADER_SIZE + pcm_size] = shared_buffer[read_offset];
        pcm_size++;
        read_offset = (read_offset + 1) % shared_data_size;
    }
    shared_read_offset = write_offset;

    unsigned int peak = 0;
    size_t sample_offset = original_pcm_size;
    if ((sample_offset & 1U) != 0) {
        sample_offset++;
    }
    while (sample_offset + 1 < pcm_size) {
        const u8 *sample_bytes = wav_buffer + WAV_HEADER_SIZE + sample_offset;
        int sample = (int)(int16_t)(
            (uint16_t)sample_bytes[0] | ((uint16_t)sample_bytes[1] << 8)
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

    if (pcm_size == PCM_CAPACITY) {
        Result result = MICU_StopSampling();
        sampling = false;
        capture_full = true;
        if (R_FAILED(result)) {
            set_result_error(error, error_capacity, "MICU_StopSampling", result);
            return false;
        }
    }

    return true;
}

bool microphone_finish_capture(char *error, size_t error_capacity)
{
    if (sampling) {
        if (!microphone_poll_capture(error, error_capacity)) {
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
                return false;
            }
        }
    }

    pcm_size &= ~(size_t)1;
    if (pcm_size == 0) {
        set_error(error, error_capacity, "no microphone samples were captured");
        return false;
    }

    finalize_wav_header();
    has_capture = true;
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

bool microphone_has_capture(void)
{
    return has_capture;
}

unsigned int microphone_duration_ms(void)
{
    return (unsigned int)(
        ((uint64_t)pcm_size * 1000U) / PCM_BYTES_PER_SECOND
    );
}

unsigned int microphone_level_percent(void)
{
    return level_percent;
}

size_t microphone_pcm_size(void)
{
    return pcm_size;
}

const void *microphone_wav_data(void)
{
    return wav_buffer;
}

size_t microphone_wav_size(void)
{
    if (!has_capture) {
        return 0;
    }
    return WAV_HEADER_SIZE + pcm_size;
}
