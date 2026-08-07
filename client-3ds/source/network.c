#include "network.h"

#include "app_config.h"

#include <3ds.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
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

static u32 *soc_buffer = NULL;
static bool soc_ready = false;
static int audio_stream_socket = -1;

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
    network_audio_stream_abort();

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
    void *progress_user_data
)
{
    char raw_response[HTTP_RESPONSE_CAPACITY + 1];
    size_t total = 0;
    size_t body_offset = 0;
    size_t delivered_body_size = 0;
    size_t expected_body_size = 0;
    bool headers_parsed = false;
    bool has_content_length = false;

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

            unsigned int status_code = 0;
            if (sscanf(raw_response, "HTTP/%*u.%*u %u", &status_code) != 1) {
                set_error(error, error_capacity, "invalid HTTP response");
                return false;
            }
            if (status_code != 200) {
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

    return true;
}

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
)
{
    if (!soc_ready) {
        set_error(error, error_capacity, "network service is not initialized");
        return false;
    }
    if (host == NULL || path == NULL || path[0] != '/'
        || content_type == NULL || (body == NULL && body_size != 0)) {
        set_error(error, error_capacity, "invalid network request");
        return false;
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &server_address.sin_addr) != 1) {
        set_error(error, error_capacity, "server host must be a numeric IPv4 address");
        return false;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) {
        set_errno_error(error, error_capacity, "socket", errno);
        return false;
    }

    bool success = false;

    do {
        if (!connect_with_timeout(
                socket_fd,
                (const struct sockaddr *)&server_address,
                sizeof(server_address),
                error,
                error_capacity
            )) {
            break;
        }

        char request_header[HTTP_REQUEST_HEADER_CAPACITY];
        int request_size = snprintf(
            request_header,
            sizeof(request_header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: 3gent/%s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            path,
            host,
            (unsigned int)port,
            THREEGENT_APP_VERSION,
            content_type,
            (unsigned int)body_size
        );

        if (request_size < 0
            || (size_t)request_size >= sizeof(request_header)) {
            set_error(
                error,
                error_capacity,
                "HTTP request header exceeded the bounded buffer"
            );
            break;
        }

        if (!send_all(
                socket_fd,
                request_header,
                (size_t)request_size,
                error,
                error_capacity
            )) {
            break;
        }

        if (body_size > 0 && !send_all(
                socket_fd,
                (const char *)body,
                body_size,
                error,
                error_capacity
            )) {
            break;
        }

        if (!read_http_response(
                socket_fd,
                response,
                response_capacity,
                error,
                error_capacity,
                progress_callback,
                progress_user_data
            )) {
            break;
        }

        success = true;
    } while (false);

    close(socket_fd);
    return success;
}

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
)
{
    if (message == NULL) {
        set_error(error, error_capacity, "invalid text request");
        return false;
    }

    return network_post_bytes(
        host,
        port,
        path,
        "text/plain; charset=utf-8",
        message,
        strlen(message),
        response,
        response_capacity,
        error,
        error_capacity,
        progress_callback,
        progress_user_data
    );
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
    if (audio_stream_socket >= 0) {
        set_error(error, error_capacity, "audio stream is already active");
        return false;
    }
    if (host == NULL || path == NULL || path[0] != '/') {
        set_error(error, error_capacity, "invalid audio stream request");
        return false;
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &server_address.sin_addr) != 1) {
        set_error(error, error_capacity, "server host must be a numeric IPv4 address");
        return false;
    }

    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) {
        set_errno_error(error, error_capacity, "socket", errno);
        return false;
    }

    if (!connect_with_timeout(
            socket_fd,
            (const struct sockaddr *)&server_address,
            sizeof(server_address),
            error,
            error_capacity
        )) {
        close(socket_fd);
        return false;
    }

    char request_header[HTTP_REQUEST_HEADER_CAPACITY];
    int request_size = snprintf(
        request_header,
        sizeof(request_header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: 3gent/%s\r\n"
        "Content-Type: application/x-3gent-pcm; "
            "format=s16le; rate=16364; channels=1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        host,
        (unsigned int)port,
        THREEGENT_APP_VERSION
    );
    if (request_size < 0
        || (size_t)request_size >= sizeof(request_header)
        || !send_all(
            socket_fd,
            request_header,
            (size_t)request_size,
            error,
            error_capacity
        )) {
        if (request_size >= 0
            && (size_t)request_size >= sizeof(request_header)) {
            set_error(
                error,
                error_capacity,
                "audio stream header exceeded the bounded buffer"
            );
        }
        close(socket_fd);
        return false;
    }

    audio_stream_socket = socket_fd;
    return true;
}

bool network_audio_stream_write(
    const void *data,
    size_t size,
    char *error,
    size_t error_capacity
)
{
    if (audio_stream_socket < 0) {
        set_error(error, error_capacity, "audio stream is not active");
        return false;
    }
    if (data == NULL || size == 0) {
        set_error(error, error_capacity, "audio stream chunk is empty");
        return false;
    }

    char chunk_header[24];
    int header_size = snprintf(
        chunk_header,
        sizeof(chunk_header),
        "%x\r\n",
        (unsigned int)size
    );
    if (header_size < 0 || (size_t)header_size >= sizeof(chunk_header)
        || !send_all(
            audio_stream_socket,
            chunk_header,
            (size_t)header_size,
            error,
            error_capacity
        )
        || !send_all(
            audio_stream_socket,
            (const char *)data,
            size,
            error,
            error_capacity
        )
        || !send_all(
            audio_stream_socket,
            "\r\n",
            2,
            error,
            error_capacity
        )) {
        network_audio_stream_abort();
        return false;
    }

    return true;
}

bool network_audio_stream_finish(
    char *response,
    size_t response_capacity,
    char *error,
    size_t error_capacity
)
{
    if (audio_stream_socket < 0) {
        set_error(error, error_capacity, "audio stream is not active");
        return false;
    }

    int socket_fd = audio_stream_socket;
    bool success = send_all(
        socket_fd,
        "0\r\n\r\n",
        5,
        error,
        error_capacity
    );
    if (success) {
        success = read_http_response(
            socket_fd,
            response,
            response_capacity,
            error,
            error_capacity,
            NULL,
            NULL
        );
    }

    close(socket_fd);
    audio_stream_socket = -1;
    return success;
}

void network_audio_stream_abort(void)
{
    if (audio_stream_socket >= 0) {
        close(audio_stream_socket);
        audio_stream_socket = -1;
    }
}
