#ifndef THREEGENT_MICROPHONE_H
#define THREEGENT_MICROPHONE_H

#include <stdbool.h>
#include <stddef.h>

#define THREEGENT_MIC_SAMPLE_RATE_HZ 16364
#define THREEGENT_MIC_CHANNELS 1
#define THREEGENT_MIC_BITS_PER_SAMPLE 16
#define THREEGENT_MIC_MAX_SECONDS 10

bool microphone_initialize(char *error, size_t error_capacity);
void microphone_shutdown(void);

bool microphone_begin_capture(char *error, size_t error_capacity);
bool microphone_poll_capture(char *error, size_t error_capacity);
bool microphone_finish_capture(char *error, size_t error_capacity);

bool microphone_is_ready(void);
bool microphone_is_sampling(void);
bool microphone_capture_is_full(void);
bool microphone_has_capture(void);

unsigned int microphone_duration_ms(void);
unsigned int microphone_level_percent(void);
size_t microphone_pcm_size(void);
const void *microphone_wav_data(void);
size_t microphone_wav_size(void);

#endif
