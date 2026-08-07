#ifndef THREEGENT_NETWORK_H
#define THREEGENT_NETWORK_H

#include <stdbool.h>
#include <stddef.h>

bool network_start(char *error, size_t error_capacity);
void network_stop(void);

bool network_echo(
    const char *host,
    unsigned short port,
    const char *message,
    char *response,
    size_t response_capacity,
    char *error,
    size_t error_capacity
);

#endif
