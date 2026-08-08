#ifndef THREEGENT_NETWORK_H
#define THREEGENT_NETWORK_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    NETWORK_OPERATION_IDLE = 0,
    NETWORK_OPERATION_IN_PROGRESS,
    NETWORK_OPERATION_SUCCEEDED,
    NETWORK_OPERATION_FAILED,
} NetworkOperationStatus;

bool network_start(char *error, size_t error_capacity);
bool network_prepare_connections(
    const char *host,
    unsigned short port,
    char *error,
    size_t error_capacity
);
unsigned int network_warm_connection_count(void);
void network_stop(void);

/*
 * Runtime control requests are advanced by network_pump(). Starting a request
 * only copies bounded request data and begins a non-blocking connection/send;
 * it never waits for the peer. Exactly one control request may be active.
 */
bool network_control_begin_get(
    const char *host,
    unsigned short port,
    const char *path,
    char *error,
    size_t error_capacity
);

bool network_control_begin_post(
    const char *host,
    unsigned short port,
    const char *path,
    const char *content_type,
    const void *body,
    size_t body_size,
    char *error,
    size_t error_capacity
);

NetworkOperationStatus network_control_status(void);
unsigned int network_control_http_status(void);
const char *network_control_response(void);
const char *network_control_error(void);
void network_control_consume(void);
void network_control_cancel(void);

/*
 * The audio request uses the second warm connection. PCM chunks are copied
 * into one bounded network queue and sent incrementally by network_pump().
 */
bool network_audio_stream_begin(
    const char *host,
    unsigned short port,
    const char *path,
    char *error,
    size_t error_capacity
);

bool network_audio_stream_can_write(void);
bool network_audio_stream_write(
    const void *data,
    size_t size,
    char *error,
    size_t error_capacity
);
bool network_audio_stream_is_ready(void);
bool network_audio_stream_finish(char *error, size_t error_capacity);
NetworkOperationStatus network_audio_stream_status(void);
const char *network_audio_stream_response(void);
const char *network_audio_stream_error(void);
void network_audio_stream_consume(void);
void network_audio_stream_abort(void);

/* Perform zero-wait progress for both control and audio network state. */
void network_pump(void);

#endif
