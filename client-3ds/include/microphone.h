#ifndef THREEGENT_MICROPHONE_H
#define THREEGENT_MICROPHONE_H

#include <stdbool.h>
#include <stddef.h>

#define THREEGENT_MIC_SAMPLE_RATE_HZ 16364
#define THREEGENT_MIC_CHANNELS 1
#define THREEGENT_MIC_BITS_PER_SAMPLE 16
#define THREEGENT_MIC_MAX_SECONDS 300

bool microphone_initialize(char *error, size_t error_capacity);
void microphone_shutdown(void);

bool microphone_begin_capture(char *error, size_t error_capacity);
bool microphone_read_capture(
    void *destination,
    size_t destination_capacity,
    size_t *bytes_read,
    char *error,
    size_t error_capacity
);
bool microphone_finish_capture(char *error, size_t error_capacity);

bool microphone_is_ready(void);
bool microphone_is_sampling(void);
bool microphone_capture_is_full(void);

unsigned int microphone_duration_ms(void);
unsigned int microphone_wall_duration_ms(void);
unsigned int microphone_level_percent(void);
size_t microphone_total_pcm_size(void);
unsigned int microphone_last_write_offset(void);
unsigned int microphone_offset_change_count(void);
unsigned int microphone_stall_ms(void);
bool microphone_service_is_sampling(void);

#endif
