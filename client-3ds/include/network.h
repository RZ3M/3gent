#ifndef THREEGENT_NETWORK_H
#define THREEGENT_NETWORK_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*NetworkProgressCallback)(const char *response, void *user_data);

bool network_start(char *error, size_t error_capacity);
bool network_prepare_connections(
    const char *host,
    unsigned short port,
    char *error,
    size_t error_capacity
);
unsigned int network_warm_connection_count(void);
void network_stop(void);

bool network_post_text(
    const char *host,
    unsigned short port,
    const char *path,
    const char *message,
    char *response,
    size_t response_capacity,
    char *error,
    size_t error_capacity,
    NetworkProgressCallback progress_callback,
    void *progress_user_data
);

bool network_get_text(
    const char *host,
    unsigned short port,
    const char *path,
    char *response,
    size_t response_capacity,
    char *error,
    size_t error_capacity
);

bool network_post_bytes(
    const char *host,
    unsigned short port,
    const char *path,
    const char *content_type,
    const void *body,
    size_t body_size,
    char *response,
    size_t response_capacity,
    char *error,
    size_t error_capacity,
    NetworkProgressCallback progress_callback,
    void *progress_user_data
);

bool network_audio_stream_begin(
    const char *host,
    unsigned short port,
    const char *path,
    char *error,
    size_t error_capacity
);

bool network_audio_stream_write(
    const void *data,
    size_t size,
    char *error,
    size_t error_capacity
);

bool network_audio_stream_finish(
    char *response,
    size_t response_capacity,
    char *error,
    size_t error_capacity
);

void network_audio_stream_abort(void);

#endif
