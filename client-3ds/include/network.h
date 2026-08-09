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

/*
 * Sets the paired device credential. When present it is sent as an
 * `Authorization: Bearer` header on every HTTP request and in the pushed
 * control hello. Pass NULL or "" to send nothing, which is what an unpaired
 * development build does.
 */
bool network_set_device_token(const char *token);
bool network_prepare_connections(
    const char *host,
    unsigned short port,
    char *error,
    size_t error_capacity
);
unsigned int network_warm_connection_count(void);

/*
 * Drops every pooled socket. Warm connections are keyed to nothing but their
 * own liveness, so they must be discarded whenever the active endpoint changes
 * — otherwise a request for the newly paired machine would be written down the
 * connection opened to the previous one.
 */
void network_reset_connections(void);
void network_stop(void);

/*
 * Development pushed-control link. It is a long-lived, non-blocking JSON-line
 * connection. One mutating command is retained until its acknowledgement so
 * reconnect can safely retry the same command ID.
 */
bool network_push_start(
    const char *host,
    unsigned short port,
    const char *session_id,
    unsigned int after,
    char *error,
    size_t error_capacity
);
bool network_push_is_ready(void);
const char *network_push_state(void);
const char *network_push_error(void);
bool network_push_has_frame(void);
const char *network_push_frame(void);
void network_push_consume_frame(void);
void network_push_set_cursor(unsigned int cursor);
bool network_push_send_text(
    const char *text,
    char *error,
    size_t error_capacity
);
bool network_push_send_interrupt(char *error, size_t error_capacity);
bool network_push_send_approval(
    const char *approval_id,
    const char *choice,
    char *error,
    size_t error_capacity
);
bool network_push_acknowledge(const char *command_id);
void network_push_stop(void);

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

/* Photo upload shares the bounded media socket/queue with audio. */
bool network_photo_upload_begin(
    const char *host,
    unsigned short port,
    const char *path,
    char *error,
    size_t error_capacity
);
bool network_photo_upload_can_write(void);
bool network_photo_upload_write(
    const void *data,
    size_t size,
    char *error,
    size_t error_capacity
);
bool network_photo_upload_finish(char *error, size_t error_capacity);
NetworkOperationStatus network_photo_upload_status(void);
const char *network_photo_upload_error(void);
void network_photo_upload_consume(void);
void network_photo_upload_abort(void);

/* Perform zero-wait progress for both control and audio network state. */
void network_pump(void);

#endif
