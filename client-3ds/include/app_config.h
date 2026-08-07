#ifndef THREEGENT_APP_CONFIG_H
#define THREEGENT_APP_CONFIG_H

#define THREEGENT_APP_VERSION "0.0.6-stage0"

/*
 * Stage 0 uses a compile-time numeric IPv4 address deliberately. Override it
 * without editing source by running:
 *
 *   make SERVER_HOST=192.168.1.42 SERVER_PORT=8080
 */
#ifndef THREEGENT_SERVER_HOST
#define THREEGENT_SERVER_HOST "192.168.1.2"
#endif
#ifndef THREEGENT_SERVER_PORT
#define THREEGENT_SERVER_PORT 8080
#endif

#define THREEGENT_PROMPT_CAPACITY 256
#define THREEGENT_RESPONSE_CAPACITY 2048
#define THREEGENT_NETWORK_TIMEOUT_SECONDS 5

#endif
