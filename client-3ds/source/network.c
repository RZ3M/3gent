#include "network.h"

#include "app_config.h"

#include <3ds.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define SOC_ALIGNMENT 0x1000
#define SOC_BUFFER_SIZE 0x100000
#define HTTP_REQUEST_HEADER_CAPACITY 1024
#define HTTP_RESPONSE_CAPACITY 4096
#define HTTP_BODY_CAPACITY 2048
#define CONTROL_BODY_CAPACITY 4096
#define NETWORK_ERROR_CAPACITY 160
#define NETWORK_HOST_CAPACITY 64
#define AUDIO_CHUNK_CAPACITY 8192
#define AUDIO_OUTPUT_CAPACITY (AUDIO_CHUNK_CAPACITY + 32)

typedef void (*NetworkProgressCallback)(
    const char *response,
    void *user_data
);

typedef enum {
    ASYNC_STEP_PENDING = 0,
    ASYNC_STEP_PROGRESSED,
    ASYNC_STEP_COMPLETE,
    ASYNC_STEP_FAILED,
} AsyncStepResult;

typedef enum {
    ASYNC_CONNECT_FAILED = 0,
    ASYNC_CONNECT_IN_PROGRESS,
    ASYNC_CONNECT_COMPLETE,
} AsyncConnectResult;

typedef struct {
    char raw[HTTP_RESPONSE_CAPACITY + 1];
    char body[HTTP_BODY_CAPACITY];
    size_t total_size;
    size_t body_offset;
    size_t expected_body_size;
    unsigned int status_code;
    bool headers_parsed;
    bool reusable;
} AsyncHttpResponse;

typedef enum {
    CONTROL_PHASE_IDLE = 0,
    CONTROL_PHASE_CONNECTING,
    CONTROL_PHASE_SENDING_HEADER,
    CONTROL_PHASE_SENDING_BODY,
    CONTROL_PHASE_RECEIVING,
    CONTROL_PHASE_SUCCEEDED,
    CONTROL_PHASE_FAILED,
} ControlPhase;

typedef struct {
    ControlPhase phase;
    char host[NETWORK_HOST_CAPACITY];
    unsigned short port;
    char request_header[HTTP_REQUEST_HEADER_CAPACITY];
    size_t request_header_size;
    size_t request_header_offset;
    char request_body[CONTROL_BODY_CAPACITY];
    size_t request_body_size;
    size_t request_body_offset;
    AsyncHttpResponse response;
    char error[NETWORK_ERROR_CAPACITY];
    u64 deadline_ms;
} ControlTransaction;

typedef enum {
    AUDIO_PHASE_IDLE = 0,
    AUDIO_PHASE_CONNECTING,
    AUDIO_PHASE_SENDING_HEADER,
    AUDIO_PHASE_STREAMING,
    AUDIO_PHASE_SENDING_END,
    AUDIO_PHASE_RECEIVING,
    AUDIO_PHASE_SUCCEEDED,
    AUDIO_PHASE_FAILED,
} AudioPhase;

typedef struct {
    AudioPhase phase;
    char host[NETWORK_HOST_CAPACITY];
    unsigned short port;
    char request_header[HTTP_REQUEST_HEADER_CAPACITY];
    size_t request_header_size;
    size_t request_header_offset;
    char output[AUDIO_OUTPUT_CAPACITY];
    size_t output_size;
    size_t output_offset;
    bool finish_requested;
    AsyncHttpResponse response;
    char error[NETWORK_ERROR_CAPACITY];
    u64 deadline_ms;
} AudioTransaction;

static u32 *soc_buffer = NULL;
static bool soc_ready = false;
static int control_socket = -1;
static int prepared_audio_socket = -1;
static int audio_stream_socket = -1;
static u32 next_command_id = 1;
static ControlTransaction control_transaction;
static AudioTransaction audio_transaction;

static void make_command_id(char *command_id, size_t capacity)
{
    u64 now_ms = osGetTime();
    snprintf(
        command_id,
        capacity,
        "cmd_3ds_%08lx%08lx_%08lx",
        (unsigned long)(now_ms >> 32),
        (unsigned long)now_ms,
        (unsigned long)next_command_id++
    );
}

static void close_socket(int *socket_fd)
{
    if (*socket_fd >= 0) {
        close(*socket_fd);
        *socket_fd = -1;
    }
}

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error == NULL || capacity == 0) {
        return;
    }

    snprintf(error, capacity, "%s", message);
}

static void set_errno_error(
    char *error,
    size_t capacity,
    const char *operation,
    int error_number
)
{
    if (error == NULL || capacity == 0) {
        return;
    }

    snprintf(
        error,
        capacity,
        "%s failed (%d: %s)",
        operation,
        error_number,
        strerror(error_number)
    );
}

bool network_start(char *error, size_t error_capacity)
{
    if (soc_ready) {
        return true;
    }

    soc_buffer = (u32 *)memalign(SOC_ALIGNMENT, SOC_BUFFER_SIZE);
    if (soc_buffer == NULL) {
        set_error(error, error_capacity, "network buffer allocation failed");
        return false;
    }

    Result result = socInit(soc_buffer, SOC_BUFFER_SIZE);
    if (R_FAILED(result)) {
        if (error != NULL && error_capacity > 0) {
            snprintf(
                error,
                error_capacity,
                "socInit failed (0x%08lx)",
                (unsigned long)result
            );
        }
        free(soc_buffer);
        soc_buffer = NULL;
        return false;
    }

    soc_ready = true;
    return true;
}

void network_stop(void)
{
    network_control_cancel();
    network_audio_stream_abort();
    close_socket(&prepared_audio_socket);
    close_socket(&control_socket);

    if (soc_ready) {
        socExit();
        soc_ready = false;
    }

    free(soc_buffer);
    soc_buffer = NULL;
}

static bool connect_with_timeout(
    int socket_fd,
    const struct sockaddr *address,
    socklen_t address_length,
    char *error,
    size_t error_capacity
)
{
    int original_flags = fcntl(socket_fd, F_GETFL, 0);
    if (original_flags < 0) {
        set_errno_error(error, error_capacity, "fcntl", errno);
        return false;
    }

    if (fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        set_errno_error(error, error_capacity, "fcntl", errno);
        return false;
    }

    int no_delay = 1;
    if (setsockopt(
            socket_fd,
            SOL_TCP,
            TCP_NODELAY,
            &no_delay,
            sizeof(no_delay)
        ) < 0) {
        set_errno_error(error, error_capacity, "TCP_NODELAY", errno);
        return false;
    }

    int result = connect(socket_fd, address, address_length);
    if (result < 0 && errno != EINPROGRESS) {
        int saved_errno = errno;
        set_errno_error(error, error_capacity, "connect", saved_errno);
        return false;
    }

    if (result < 0) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_fd, &write_set);

        struct timeval timeout = {
            .tv_sec = THREEGENT_NETWORK_TIMEOUT_SECONDS,
            .tv_usec = 0,
        };

        result = select(socket_fd + 1, NULL, &write_set, NULL, &timeout);
        if (result == 0) {
            set_error(error, error_capacity, "connect timed out");
            return false;
        }
        if (result < 0) {
            int saved_errno = errno;
            set_errno_error(error, error_capacity, "select", saved_errno);
            return false;
        }

        if (!FD_ISSET(socket_fd, &write_set)) {
            set_error(error, error_capacity, "connect did not become writable");
            return false;
        }

        /*
         * On libctru 2.7.0 hardware, SO_ERROR can expose the SOC service's
         * unconverted -26 (EINPROGRESS) value even after the peer accepted the
         * connection. A writable socket is allowed to continue here; the first
         * bounded send reports any real connection failure.
         */
    }

    /* Leave the socket non-blocking so all later waits remain bounded. */
    return true;
}

static bool wait_for_socket(
    int socket_fd,
    bool wait_for_write,
    const char *operation,
    char *error,
    size_t error_capacity
)
{
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);

    if (wait_for_write) {
        FD_SET(socket_fd, &write_set);
    } else {
        FD_SET(socket_fd, &read_set);
    }

    struct timeval timeout = {
        .tv_sec = THREEGENT_NETWORK_TIMEOUT_SECONDS,
        .tv_usec = 0,
    };

    int result = select(
        socket_fd + 1,
        wait_for_write ? NULL : &read_set,
        wait_for_write ? &write_set : NULL,
        NULL,
        &timeout
    );
    if (result == 0) {
        if (error != NULL && error_capacity > 0) {
            snprintf(error, error_capacity, "%s timed out", operation);
        }
        return false;
    }
    if (result < 0) {
        set_errno_error(error, error_capacity, "select", errno);
        return false;
    }

    return true;
}

static bool send_all(
    int socket_fd,
    const char *data,
    size_t size,
    char *error,
    size_t error_capacity
)
{
    size_t sent = 0;

    while (sent < size) {
        if (!wait_for_socket(
                socket_fd,
                true,
                "send",
                error,
                error_capacity
            )) {
            return false;
        }

        ssize_t result = send(socket_fd, data + sent, size - sent, 0);
        if (result == 0) {
            set_error(error, error_capacity, "connection closed during send");
            return false;
        }
        if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            set_errno_error(error, error_capacity, "send", errno);
            return false;
        }
        sent += (size_t)result;
    }

    return true;
}

static bool read_http_response(
    int socket_fd,
    char *response,
    size_t response_capacity,
    char *error,
    size_t error_capacity,
    NetworkProgressCallback progress_callback,
    void *progress_user_data,
    bool *connection_reusable
)
{
    char raw_response[HTTP_RESPONSE_CAPACITY + 1];
    size_t total = 0;
    size_t body_offset = 0;
    size_t delivered_body_size = 0;
    size_t expected_body_size = 0;
    bool headers_parsed = false;
    bool has_content_length = false;
    bool reusable = false;

    if (connection_reusable != NULL) {
        *connection_reusable = false;
    }

    if (response == NULL || response_capacity == 0) {
        set_error(error, error_capacity, "response buffer is unavailable");
        return false;
    }
    response[0] = '\0';

    while (total < HTTP_RESPONSE_CAPACITY) {
        if (!wait_for_socket(
                socket_fd,
                false,
                "receive",
                error,
                error_capacity
            )) {
            return false;
        }

        ssize_t received = recv(
            socket_fd,
            raw_response + total,
            HTTP_RESPONSE_CAPACITY - total,
            0
        );

        if (received == 0) {
            break;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            set_errno_error(error, error_capacity, "receive", errno);
            return false;
        }

        total += (size_t)received;
        raw_response[total] = '\0';

        if (!headers_parsed) {
            char *header_end = strstr(raw_response, "\r\n\r\n");
            if (header_end == NULL) {
                continue;
            }

            unsigned int http_major = 0;
            unsigned int http_minor = 0;
            unsigned int status_code = 0;
            if (sscanf(
                    raw_response,
                    "HTTP/%u.%u %u",
                    &http_major,
                    &http_minor,
                    &status_code
                ) != 3) {
                set_error(error, error_capacity, "invalid HTTP response");
                return false;
            }
            if (status_code < 200 || status_code >= 300) {
                if (error != NULL && error_capacity > 0) {
                    snprintf(
                        error,
                        error_capacity,
                        "server returned HTTP %u",
                        status_code
                    );
                }
                return false;
            }

            reusable = http_major > 1
                || (http_major == 1 && http_minor >= 1);
            if (strstr(raw_response, "\r\nConnection: close") != NULL) {
                reusable = false;
            } else if (strstr(
                    raw_response,
                    "\r\nConnection: keep-alive"
                ) != NULL) {
                reusable = true;
            }

            char *content_length = strstr(raw_response, "\r\nContent-Length:");
            if (content_length != NULL) {
                unsigned long parsed_length = 0;
                if (sscanf(
                        content_length,
                        "\r\nContent-Length: %lu",
                        &parsed_length
                    ) != 1) {
                    set_error(error, error_capacity, "invalid Content-Length");
                    return false;
                }
                if (parsed_length >= response_capacity) {
                    set_error(
                        error,
                        error_capacity,
                        "response exceeded the bounded buffer"
                    );
                    return false;
                }
                expected_body_size = (size_t)parsed_length;
                has_content_length = true;
            }

            body_offset = (size_t)(header_end - raw_response) + 4;
            headers_parsed = true;
        }

        if (headers_parsed) {
            size_t available_body_size = total - body_offset;
            if (available_body_size > delivered_body_size) {
                size_t new_body_size = available_body_size - delivered_body_size;
                if (delivered_body_size + new_body_size >= response_capacity) {
                    set_error(
                        error,
                        error_capacity,
                        "response exceeded the bounded buffer"
                    );
                    return false;
                }

                memcpy(
                    response + delivered_body_size,
                    raw_response + body_offset + delivered_body_size,
                    new_body_size
                );
                delivered_body_size += new_body_size;
                response[delivered_body_size] = '\0';

                if (progress_callback != NULL) {
                    progress_callback(response, progress_user_data);
                }
            }

            if (has_content_length
                && delivered_body_size == expected_body_size) {
                break;
            }
        }
    }

    if (total == HTTP_RESPONSE_CAPACITY) {
        set_error(error, error_capacity, "HTTP response exceeded the raw buffer");
        return false;
    }
    if (!headers_parsed) {
        set_error(error, error_capacity, "HTTP response has no body separator");
        return false;
    }

    if (has_content_length && delivered_body_size != expected_body_size) {
        if (error != NULL && error_capacity > 0) {
            snprintf(
                error,
                error_capacity,
                "response ended early (%u/%u bytes)",
                (unsigned int)delivered_body_size,
                (unsigned int)expected_body_size
            );
        }
        return false;
    }

    if (connection_reusable != NULL) {
        *connection_reusable = reusable && has_content_length;
    }

    return true;
}

static int open_server_socket(
    const char *host,
    unsigned short port,
    char *error,
    size_t error_capacity
)
{
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server_address.sin_addr) != 1) {
        set_error(
            error,
            error_capacity,
            "server host must be a numeric IPv4 address"
        );
        return -1;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) {
        set_errno_error(error, error_capacity, "socket", errno);
        return -1;
    }

    if (!connect_with_timeout(
            socket_fd,
            (const struct sockaddr *)&server_address,
            sizeof(server_address),
            error,
            error_capacity
        )) {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static bool verify_persistent_connection(
    int socket_fd,
    const char *host,
    unsigned short port,
    char *error,
    size_t error_capacity
)
{
    char request[256];
    int request_size = snprintf(
        request,
        sizeof(request),
        "GET /health HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: 3gent/%s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        host,
        (unsigned int)port,
        THREEGENT_APP_VERSION
    );
    if (request_size < 0 || (size_t)request_size >= sizeof(request)) {
        set_error(error, error_capacity, "health request is too large");
        return false;
    }
    if (!send_all(
            socket_fd,
            request,
            (size_t)request_size,
            error,
            error_capacity
        )) {
        return false;
    }

    char response[80];
    bool reusable = false;
    if (!read_http_response(
            socket_fd,
            response,
            sizeof(response),
            error,
            error_capacity,
            NULL,
            NULL,
            &reusable
        )) {
        return false;
    }
    if (!reusable) {
        set_error(
            error,
            error_capacity,
            "server closed warm connection; restart updated server"
        );
        return false;
    }
    return true;
}

bool network_prepare_connections(
    const char *host,
    unsigned short port,
    char *error,
    size_t error_capacity
)
{
    if (!soc_ready) {
        set_error(error, error_capacity, "network service is not initialized");
        return false;
    }
    if (host == NULL) {
        set_error(error, error_capacity, "invalid server host");
        return false;
    }
    if (audio_stream_socket >= 0) {
        set_error(error, error_capacity, "audio stream is active");
        return false;
    }

    close_socket(&control_socket);
    close_socket(&prepared_audio_socket);

    int new_control_socket = open_server_socket(
        host,
        port,
        error,
        error_capacity
    );
    if (new_control_socket < 0) {
        return false;
    }

    int new_audio_socket = open_server_socket(
        host,
        port,
        error,
        error_capacity
    );
    if (new_audio_socket < 0) {
        close(new_control_socket);
        return false;
    }

    bool control_ready = verify_persistent_connection(
        new_control_socket,
        host,
        port,
        error,
        error_capacity
    );
    bool audio_ready = control_ready && verify_persistent_connection(
        new_audio_socket,
        host,
        port,
        error,
        error_capacity
    );
    if (!control_ready || !audio_ready) {
        close(new_audio_socket);
        close(new_control_socket);
        return false;
    }

    control_socket = new_control_socket;
    prepared_audio_socket = new_audio_socket;
    return true;
}

unsigned int network_warm_connection_count(void)
{
    unsigned int count = control_socket >= 0 ? 1U : 0U;
    if (prepared_audio_socket >= 0 || audio_stream_socket >= 0) {
        count++;
    }
    return count;
}

static void reset_async_response(AsyncHttpResponse *response)
{
    memset(response, 0, sizeof(*response));
    response->body[0] = '\0';
}

static bool deadline_expired(u64 deadline_ms)
{
    return deadline_ms != 0 && osGetTime() >= deadline_ms;
}

static u64 new_network_deadline(void)
{
    return osGetTime() + (u64)THREEGENT_NETWORK_TIMEOUT_SECONDS * 1000U;
}

static int socket_ready_now(
    int socket_fd,
    bool wait_for_write,
    char *error,
    size_t error_capacity
)
{
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (wait_for_write) {
        FD_SET(socket_fd, &write_set);
    } else {
        FD_SET(socket_fd, &read_set);
    }

    struct timeval timeout = {.tv_sec = 0, .tv_usec = 0};
    int result = select(
        socket_fd + 1,
        wait_for_write ? NULL : &read_set,
        wait_for_write ? &write_set : NULL,
        NULL,
        &timeout
    );
    if (result < 0) {
        set_errno_error(error, error_capacity, "select", errno);
        return -1;
    }
    return result > 0 ? 1 : 0;
}

static AsyncConnectResult start_async_server_socket(
    const char *host,
    unsigned short port,
    int *socket_fd,
    char *error,
    size_t error_capacity
)
{
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &server_address.sin_addr) != 1) {
        set_error(error, error_capacity, "server host must be a numeric IPv4 address");
        return ASYNC_CONNECT_FAILED;
    }

    int new_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (new_socket < 0) {
        set_errno_error(error, error_capacity, "socket", errno);
        return ASYNC_CONNECT_FAILED;
    }

    int flags = fcntl(new_socket, F_GETFL, 0);
    if (flags < 0 || fcntl(new_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
        set_errno_error(error, error_capacity, "fcntl", errno);
        close(new_socket);
        return ASYNC_CONNECT_FAILED;
    }

    int no_delay = 1;
    if (setsockopt(
            new_socket,
            SOL_TCP,
            TCP_NODELAY,
            &no_delay,
            sizeof(no_delay)
        ) < 0) {
        set_errno_error(error, error_capacity, "TCP_NODELAY", errno);
        close(new_socket);
        return ASYNC_CONNECT_FAILED;
    }

    int result = connect(
        new_socket,
        (const struct sockaddr *)&server_address,
        sizeof(server_address)
    );
    if (result < 0 && errno != EINPROGRESS) {
        set_errno_error(error, error_capacity, "connect", errno);
        close(new_socket);
        return ASYNC_CONNECT_FAILED;
    }

    *socket_fd = new_socket;
    return result == 0
        ? ASYNC_CONNECT_COMPLETE
        : ASYNC_CONNECT_IN_PROGRESS;
}

static AsyncStepResult send_bytes_step(
    int socket_fd,
    const char *data,
    size_t size,
    size_t *offset,
    char *error,
    size_t error_capacity
)
{
    if (*offset >= size) {
        return ASYNC_STEP_COMPLETE;
    }

    ssize_t sent = send(socket_fd, data + *offset, size - *offset, 0);
    if (sent > 0) {
        *offset += (size_t)sent;
        return *offset == size
            ? ASYNC_STEP_COMPLETE
            : ASYNC_STEP_PROGRESSED;
    }
    if (sent == 0) {
        set_error(error, error_capacity, "connection closed during send");
        return ASYNC_STEP_FAILED;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return ASYNC_STEP_PENDING;
    }
    set_errno_error(error, error_capacity, "send", errno);
    return ASYNC_STEP_FAILED;
}

static AsyncStepResult receive_http_step(
    int socket_fd,
    AsyncHttpResponse *response,
    char *error,
    size_t error_capacity
)
{
    if (response->total_size >= HTTP_RESPONSE_CAPACITY) {
        set_error(error, error_capacity, "HTTP response exceeded the raw buffer");
        return ASYNC_STEP_FAILED;
    }

    ssize_t received = recv(
        socket_fd,
        response->raw + response->total_size,
        HTTP_RESPONSE_CAPACITY - response->total_size,
        0
    );
    if (received == 0) {
        set_error(error, error_capacity, "connection closed during receive");
        return ASYNC_STEP_FAILED;
    }
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return ASYNC_STEP_PENDING;
        }
        set_errno_error(error, error_capacity, "receive", errno);
        return ASYNC_STEP_FAILED;
    }

    response->total_size += (size_t)received;
    response->raw[response->total_size] = '\0';

    if (!response->headers_parsed) {
        char *header_end = strstr(response->raw, "\r\n\r\n");
        if (header_end == NULL) {
            return response->total_size == HTTP_RESPONSE_CAPACITY
                ? ASYNC_STEP_FAILED
                : ASYNC_STEP_PROGRESSED;
        }

        unsigned int http_major = 0;
        unsigned int http_minor = 0;
        if (sscanf(
                response->raw,
                "HTTP/%u.%u %u",
                &http_major,
                &http_minor,
                &response->status_code
            ) != 3) {
            set_error(error, error_capacity, "invalid HTTP response");
            return ASYNC_STEP_FAILED;
        }

        response->reusable = http_major > 1
            || (http_major == 1 && http_minor >= 1);
        if (strstr(response->raw, "\r\nConnection: close") != NULL) {
            response->reusable = false;
        } else if (strstr(
                response->raw,
                "\r\nConnection: keep-alive"
            ) != NULL) {
            response->reusable = true;
        }

        char *content_length = strstr(response->raw, "\r\nContent-Length:");
        unsigned long parsed_length = 0;
        if (content_length == NULL || sscanf(
                content_length,
                "\r\nContent-Length: %lu",
                &parsed_length
            ) != 1) {
            set_error(error, error_capacity, "HTTP response requires Content-Length");
            return ASYNC_STEP_FAILED;
        }
        if (parsed_length >= sizeof(response->body)) {
            set_error(error, error_capacity, "response exceeded the bounded buffer");
            return ASYNC_STEP_FAILED;
        }

        response->expected_body_size = (size_t)parsed_length;
        response->body_offset = (size_t)(header_end - response->raw) + 4;
        response->headers_parsed = true;
    }

    size_t available_body_size = response->total_size - response->body_offset;
    if (available_body_size > response->expected_body_size) {
        set_error(error, error_capacity, "HTTP response exceeded Content-Length");
        return ASYNC_STEP_FAILED;
    }
    if (available_body_size == response->expected_body_size) {
        memcpy(
            response->body,
            response->raw + response->body_offset,
            available_body_size
        );
        response->body[available_body_size] = '\0';
        return ASYNC_STEP_COMPLETE;
    }
    return ASYNC_STEP_PROGRESSED;
}

static void fail_control(const char *fallback_message)
{
    if (control_transaction.error[0] == '\0') {
        set_error(
            control_transaction.error,
            sizeof(control_transaction.error),
            fallback_message
        );
    }
    close_socket(&control_socket);
    control_transaction.phase = CONTROL_PHASE_FAILED;
    control_transaction.deadline_ms = 0;
}

static bool begin_control_request(
    const char *host,
    unsigned short port,
    const char *path,
    const char *method,
    const char *content_type,
    const void *body,
    size_t body_size,
    char *error,
    size_t error_capacity
)
{
    if (!soc_ready) {
        set_error(error, error_capacity, "network service is not initialized");
        return false;
    }
    if (control_transaction.phase != CONTROL_PHASE_IDLE) {
        set_error(error, error_capacity, "control request is already active");
        return false;
    }
    if (host == NULL || strlen(host) >= sizeof(control_transaction.host)
        || path == NULL || path[0] != '/' || method == NULL
        || (body == NULL && body_size != 0)
        || body_size > sizeof(control_transaction.request_body)) {
        set_error(error, error_capacity, "invalid or oversized control request");
        return false;
    }

    memset(&control_transaction, 0, sizeof(control_transaction));
    snprintf(control_transaction.host, sizeof(control_transaction.host), "%s", host);
    control_transaction.port = port;
    if (body_size > 0) {
        memcpy(control_transaction.request_body, body, body_size);
    }
    control_transaction.request_body_size = body_size;
    reset_async_response(&control_transaction.response);

    int request_size = 0;
    if (strcmp(method, "GET") == 0) {
        request_size = snprintf(
            control_transaction.request_header,
            sizeof(control_transaction.request_header),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: 3gent/%s\r\n"
            "X-3gent-Protocol-Version: %u\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            path,
            host,
            (unsigned int)port,
            THREEGENT_APP_VERSION,
            THREEGENT_PROTOCOL_VERSION
        );
    } else {
        char command_id[48];
        make_command_id(command_id, sizeof(command_id));
        request_size = snprintf(
            control_transaction.request_header,
            sizeof(control_transaction.request_header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: 3gent/%s\r\n"
            "X-3gent-Protocol-Version: %u\r\n"
            "X-3gent-Command-Id: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %u\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            path,
            host,
            (unsigned int)port,
            THREEGENT_APP_VERSION,
            THREEGENT_PROTOCOL_VERSION,
            command_id,
            content_type,
            (unsigned int)body_size
        );
    }
    if (request_size < 0
        || (size_t)request_size >= sizeof(control_transaction.request_header)) {
        memset(&control_transaction, 0, sizeof(control_transaction));
        set_error(error, error_capacity, "control request header exceeded its buffer");
        return false;
    }
    control_transaction.request_header_size = (size_t)request_size;
    control_transaction.deadline_ms = new_network_deadline();

    if (control_socket >= 0) {
        control_transaction.phase = CONTROL_PHASE_SENDING_HEADER;
        return true;
    }

    AsyncConnectResult connect_result = start_async_server_socket(
        host,
        port,
        &control_socket,
        control_transaction.error,
        sizeof(control_transaction.error)
    );
    if (connect_result == ASYNC_CONNECT_FAILED) {
        set_error(error, error_capacity, control_transaction.error);
        memset(&control_transaction, 0, sizeof(control_transaction));
        return false;
    }
    control_transaction.phase = connect_result == ASYNC_CONNECT_COMPLETE
        ? CONTROL_PHASE_SENDING_HEADER
        : CONTROL_PHASE_CONNECTING;
    return true;
}

bool network_control_begin_get(
    const char *host,
    unsigned short port,
    const char *path,
    char *error,
    size_t error_capacity
)
{
    return begin_control_request(
        host,
        port,
        path,
        "GET",
        NULL,
        NULL,
        0,
        error,
        error_capacity
    );
}

bool network_control_begin_post(
    const char *host,
    unsigned short port,
    const char *path,
    const char *content_type,
    const void *body,
    size_t body_size,
    char *error,
    size_t error_capacity
)
{
    if (content_type == NULL) {
        set_error(error, error_capacity, "control content type is unavailable");
        return false;
    }
    return begin_control_request(
        host,
        port,
        path,
        "POST",
        content_type,
        body,
        body_size,
        error,
        error_capacity
    );
}

static void pump_control(void)
{
    for (unsigned int transitions = 0; transitions < 4; transitions++) {
        if (control_transaction.phase == CONTROL_PHASE_IDLE
            || control_transaction.phase == CONTROL_PHASE_SUCCEEDED
            || control_transaction.phase == CONTROL_PHASE_FAILED) {
            return;
        }
        if (deadline_expired(control_transaction.deadline_ms)) {
            set_error(
                control_transaction.error,
                sizeof(control_transaction.error),
                "control request timed out"
            );
            fail_control("control request timed out");
            return;
        }

        if (control_transaction.phase == CONTROL_PHASE_CONNECTING) {
            int ready = socket_ready_now(
                control_socket,
                true,
                control_transaction.error,
                sizeof(control_transaction.error)
            );
            if (ready < 0) {
                fail_control("control connect failed");
                return;
            }
            if (ready == 0) {
                return;
            }
            /* See connect_with_timeout(): SO_ERROR is unreliable on hardware. */
            control_transaction.phase = CONTROL_PHASE_SENDING_HEADER;
            control_transaction.deadline_ms = new_network_deadline();
            continue;
        }

        if (control_transaction.phase == CONTROL_PHASE_SENDING_HEADER) {
            AsyncStepResult step = send_bytes_step(
                control_socket,
                control_transaction.request_header,
                control_transaction.request_header_size,
                &control_transaction.request_header_offset,
                control_transaction.error,
                sizeof(control_transaction.error)
            );
            if (step == ASYNC_STEP_FAILED) {
                fail_control("control header send failed");
                return;
            }
            if (step == ASYNC_STEP_PENDING) {
                return;
            }
            control_transaction.deadline_ms = new_network_deadline();
            if (step == ASYNC_STEP_COMPLETE) {
                control_transaction.phase = control_transaction.request_body_size > 0
                    ? CONTROL_PHASE_SENDING_BODY
                    : CONTROL_PHASE_RECEIVING;
                continue;
            }
            return;
        }

        if (control_transaction.phase == CONTROL_PHASE_SENDING_BODY) {
            AsyncStepResult step = send_bytes_step(
                control_socket,
                control_transaction.request_body,
                control_transaction.request_body_size,
                &control_transaction.request_body_offset,
                control_transaction.error,
                sizeof(control_transaction.error)
            );
            if (step == ASYNC_STEP_FAILED) {
                fail_control("control body send failed");
                return;
            }
            if (step == ASYNC_STEP_PENDING) {
                return;
            }
            control_transaction.deadline_ms = new_network_deadline();
            if (step == ASYNC_STEP_COMPLETE) {
                control_transaction.phase = CONTROL_PHASE_RECEIVING;
                continue;
            }
            return;
        }

        AsyncStepResult step = receive_http_step(
            control_socket,
            &control_transaction.response,
            control_transaction.error,
            sizeof(control_transaction.error)
        );
        if (step == ASYNC_STEP_FAILED) {
            fail_control("control receive failed");
            return;
        }
        if (step == ASYNC_STEP_PENDING) {
            return;
        }
        control_transaction.deadline_ms = new_network_deadline();
        if (step != ASYNC_STEP_COMPLETE) {
            return;
        }

        unsigned int status = control_transaction.response.status_code;
        if (status < 200 || status >= 300) {
            snprintf(
                control_transaction.error,
                sizeof(control_transaction.error),
                "server returned HTTP %u",
                status
            );
            fail_control("control request failed");
            return;
        }
        if (!control_transaction.response.reusable) {
            close_socket(&control_socket);
        }
        control_transaction.phase = CONTROL_PHASE_SUCCEEDED;
        control_transaction.deadline_ms = 0;
        return;
    }
}

NetworkOperationStatus network_control_status(void)
{
    switch (control_transaction.phase) {
        case CONTROL_PHASE_IDLE:
            return NETWORK_OPERATION_IDLE;
        case CONTROL_PHASE_SUCCEEDED:
            return NETWORK_OPERATION_SUCCEEDED;
        case CONTROL_PHASE_FAILED:
            return NETWORK_OPERATION_FAILED;
        default:
            return NETWORK_OPERATION_IN_PROGRESS;
    }
}

unsigned int network_control_http_status(void)
{
    return control_transaction.response.status_code;
}

const char *network_control_response(void)
{
    return control_transaction.response.body;
}

const char *network_control_error(void)
{
    return control_transaction.error;
}

void network_control_consume(void)
{
    if (control_transaction.phase == CONTROL_PHASE_SUCCEEDED
        || control_transaction.phase == CONTROL_PHASE_FAILED) {
        memset(&control_transaction, 0, sizeof(control_transaction));
    }
}

void network_control_cancel(void)
{
    if (network_control_status() == NETWORK_OPERATION_IN_PROGRESS) {
        close_socket(&control_socket);
    }
    memset(&control_transaction, 0, sizeof(control_transaction));
}

static void fail_audio(const char *fallback_message)
{
    if (audio_transaction.error[0] == '\0') {
        set_error(
            audio_transaction.error,
            sizeof(audio_transaction.error),
            fallback_message
        );
    }
    close_socket(&audio_stream_socket);
    audio_transaction.phase = AUDIO_PHASE_FAILED;
    audio_transaction.deadline_ms = 0;
}

bool network_audio_stream_begin(
    const char *host,
    unsigned short port,
    const char *path,
    char *error,
    size_t error_capacity
)
{
    if (!soc_ready) {
        set_error(error, error_capacity, "network service is not initialized");
        return false;
    }
    if (audio_transaction.phase != AUDIO_PHASE_IDLE) {
        set_error(error, error_capacity, "audio stream is already active");
        return false;
    }
    if (host == NULL || strlen(host) >= sizeof(audio_transaction.host)
        || path == NULL || path[0] != '/') {
        set_error(error, error_capacity, "invalid audio stream request");
        return false;
    }

    memset(&audio_transaction, 0, sizeof(audio_transaction));
    snprintf(audio_transaction.host, sizeof(audio_transaction.host), "%s", host);
    audio_transaction.port = port;
    reset_async_response(&audio_transaction.response);

    char command_id[48];
    make_command_id(command_id, sizeof(command_id));
    int request_size = snprintf(
        audio_transaction.request_header,
        sizeof(audio_transaction.request_header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: 3gent/%s\r\n"
        "X-3gent-Protocol-Version: %u\r\n"
        "X-3gent-Command-Id: %s\r\n"
        "Content-Type: application/x-3gent-pcm; "
            "format=s16le; rate=16364; channels=1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path,
        host,
        (unsigned int)port,
        THREEGENT_APP_VERSION,
        THREEGENT_PROTOCOL_VERSION,
        command_id
    );
    if (request_size < 0
        || (size_t)request_size >= sizeof(audio_transaction.request_header)) {
        memset(&audio_transaction, 0, sizeof(audio_transaction));
        set_error(error, error_capacity, "audio stream header exceeded its buffer");
        return false;
    }
    audio_transaction.request_header_size = (size_t)request_size;
    audio_transaction.deadline_ms = new_network_deadline();

    audio_stream_socket = prepared_audio_socket;
    prepared_audio_socket = -1;
    if (audio_stream_socket >= 0) {
        audio_transaction.phase = AUDIO_PHASE_SENDING_HEADER;
        return true;
    }

    AsyncConnectResult connect_result = start_async_server_socket(
        host,
        port,
        &audio_stream_socket,
        audio_transaction.error,
        sizeof(audio_transaction.error)
    );
    if (connect_result == ASYNC_CONNECT_FAILED) {
        set_error(error, error_capacity, audio_transaction.error);
        memset(&audio_transaction, 0, sizeof(audio_transaction));
        return false;
    }
    audio_transaction.phase = connect_result == ASYNC_CONNECT_COMPLETE
        ? AUDIO_PHASE_SENDING_HEADER
        : AUDIO_PHASE_CONNECTING;
    return true;
}

bool network_audio_stream_can_write(void)
{
    return audio_transaction.phase == AUDIO_PHASE_STREAMING
        && audio_transaction.output_size == 0
        && !audio_transaction.finish_requested;
}

bool network_audio_stream_write(
    const void *data,
    size_t size,
    char *error,
    size_t error_capacity
)
{
    if (!network_audio_stream_can_write()) {
        set_error(error, error_capacity, "audio network queue is busy");
        return false;
    }
    if (data == NULL || size == 0 || size > AUDIO_CHUNK_CAPACITY) {
        set_error(error, error_capacity, "audio stream chunk is invalid");
        return false;
    }

    int header_size = snprintf(
        audio_transaction.output,
        sizeof(audio_transaction.output),
        "%x\r\n",
        (unsigned int)size
    );
    if (header_size < 0
        || (size_t)header_size + size + 2 > sizeof(audio_transaction.output)) {
        set_error(error, error_capacity, "audio stream chunk exceeded its buffer");
        return false;
    }
    memcpy(audio_transaction.output + header_size, data, size);
    memcpy(audio_transaction.output + header_size + size, "\r\n", 2);
    audio_transaction.output_size = (size_t)header_size + size + 2;
    audio_transaction.output_offset = 0;
    audio_transaction.deadline_ms = new_network_deadline();
    return true;
}

bool network_audio_stream_is_ready(void)
{
    return audio_transaction.phase == AUDIO_PHASE_STREAMING;
}

bool network_audio_stream_finish(char *error, size_t error_capacity)
{
    NetworkOperationStatus status = network_audio_stream_status();
    if (status != NETWORK_OPERATION_IN_PROGRESS) {
        set_error(error, error_capacity, "audio stream is not active");
        return false;
    }
    audio_transaction.finish_requested = true;
    return true;
}

static void pump_audio(void)
{
    for (unsigned int transitions = 0; transitions < 4; transitions++) {
        if (audio_transaction.phase == AUDIO_PHASE_IDLE
            || audio_transaction.phase == AUDIO_PHASE_SUCCEEDED
            || audio_transaction.phase == AUDIO_PHASE_FAILED) {
            return;
        }

        bool requires_deadline = audio_transaction.phase != AUDIO_PHASE_STREAMING
            || audio_transaction.output_size > 0
            || audio_transaction.finish_requested;
        if (requires_deadline && deadline_expired(audio_transaction.deadline_ms)) {
            set_error(
                audio_transaction.error,
                sizeof(audio_transaction.error),
                "audio stream timed out"
            );
            fail_audio("audio stream timed out");
            return;
        }

        if (audio_transaction.phase == AUDIO_PHASE_CONNECTING) {
            int ready = socket_ready_now(
                audio_stream_socket,
                true,
                audio_transaction.error,
                sizeof(audio_transaction.error)
            );
            if (ready < 0) {
                fail_audio("audio connect failed");
                return;
            }
            if (ready == 0) {
                return;
            }
            audio_transaction.phase = AUDIO_PHASE_SENDING_HEADER;
            audio_transaction.deadline_ms = new_network_deadline();
            continue;
        }

        if (audio_transaction.phase == AUDIO_PHASE_SENDING_HEADER) {
            AsyncStepResult step = send_bytes_step(
                audio_stream_socket,
                audio_transaction.request_header,
                audio_transaction.request_header_size,
                &audio_transaction.request_header_offset,
                audio_transaction.error,
                sizeof(audio_transaction.error)
            );
            if (step == ASYNC_STEP_FAILED) {
                fail_audio("audio header send failed");
                return;
            }
            if (step == ASYNC_STEP_PENDING) {
                return;
            }
            audio_transaction.deadline_ms = new_network_deadline();
            if (step == ASYNC_STEP_COMPLETE) {
                audio_transaction.phase = AUDIO_PHASE_STREAMING;
                continue;
            }
            return;
        }

        if (audio_transaction.phase == AUDIO_PHASE_STREAMING) {
            if (audio_transaction.output_size > 0) {
                AsyncStepResult step = send_bytes_step(
                    audio_stream_socket,
                    audio_transaction.output,
                    audio_transaction.output_size,
                    &audio_transaction.output_offset,
                    audio_transaction.error,
                    sizeof(audio_transaction.error)
                );
                if (step == ASYNC_STEP_FAILED) {
                    fail_audio("audio chunk send failed");
                    return;
                }
                if (step == ASYNC_STEP_PENDING) {
                    return;
                }
                audio_transaction.deadline_ms = new_network_deadline();
                if (step == ASYNC_STEP_COMPLETE) {
                    audio_transaction.output_size = 0;
                    audio_transaction.output_offset = 0;
                    continue;
                }
                return;
            }
            if (!audio_transaction.finish_requested) {
                audio_transaction.deadline_ms = 0;
                return;
            }
            memcpy(audio_transaction.output, "0\r\n\r\n", 5);
            audio_transaction.output_size = 5;
            audio_transaction.output_offset = 0;
            audio_transaction.phase = AUDIO_PHASE_SENDING_END;
            audio_transaction.deadline_ms = new_network_deadline();
            continue;
        }

        if (audio_transaction.phase == AUDIO_PHASE_SENDING_END) {
            AsyncStepResult step = send_bytes_step(
                audio_stream_socket,
                audio_transaction.output,
                audio_transaction.output_size,
                &audio_transaction.output_offset,
                audio_transaction.error,
                sizeof(audio_transaction.error)
            );
            if (step == ASYNC_STEP_FAILED) {
                fail_audio("audio final marker send failed");
                return;
            }
            if (step == ASYNC_STEP_PENDING) {
                return;
            }
            audio_transaction.deadline_ms = new_network_deadline();
            if (step == ASYNC_STEP_COMPLETE) {
                audio_transaction.output_size = 0;
                audio_transaction.output_offset = 0;
                reset_async_response(&audio_transaction.response);
                audio_transaction.phase = AUDIO_PHASE_RECEIVING;
                continue;
            }
            return;
        }

        AsyncStepResult step = receive_http_step(
            audio_stream_socket,
            &audio_transaction.response,
            audio_transaction.error,
            sizeof(audio_transaction.error)
        );
        if (step == ASYNC_STEP_FAILED) {
            fail_audio("audio response failed");
            return;
        }
        if (step == ASYNC_STEP_PENDING) {
            return;
        }
        audio_transaction.deadline_ms = new_network_deadline();
        if (step != ASYNC_STEP_COMPLETE) {
            return;
        }

        unsigned int status = audio_transaction.response.status_code;
        if (status < 200 || status >= 300) {
            snprintf(
                audio_transaction.error,
                sizeof(audio_transaction.error),
                "server returned HTTP %u",
                status
            );
            fail_audio("audio stream failed");
            return;
        }
        if (audio_transaction.response.reusable) {
            prepared_audio_socket = audio_stream_socket;
            audio_stream_socket = -1;
        } else {
            close_socket(&audio_stream_socket);
        }
        audio_transaction.phase = AUDIO_PHASE_SUCCEEDED;
        audio_transaction.deadline_ms = 0;
        return;
    }
}

NetworkOperationStatus network_audio_stream_status(void)
{
    switch (audio_transaction.phase) {
        case AUDIO_PHASE_IDLE:
            return NETWORK_OPERATION_IDLE;
        case AUDIO_PHASE_SUCCEEDED:
            return NETWORK_OPERATION_SUCCEEDED;
        case AUDIO_PHASE_FAILED:
            return NETWORK_OPERATION_FAILED;
        default:
            return NETWORK_OPERATION_IN_PROGRESS;
    }
}

const char *network_audio_stream_response(void)
{
    return audio_transaction.response.body;
}

const char *network_audio_stream_error(void)
{
    return audio_transaction.error;
}

void network_audio_stream_consume(void)
{
    if (audio_transaction.phase == AUDIO_PHASE_SUCCEEDED
        || audio_transaction.phase == AUDIO_PHASE_FAILED) {
        memset(&audio_transaction, 0, sizeof(audio_transaction));
    }
}

void network_audio_stream_abort(void)
{
    close_socket(&audio_stream_socket);
    memset(&audio_transaction, 0, sizeof(audio_transaction));
}

void network_pump(void)
{
    pump_control();
    pump_audio();
}
