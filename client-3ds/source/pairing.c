#include "pairing.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PAIRING_DIRECTORY "sdmc:/3ds/3gent"
#define PAIRING_FILE PAIRING_DIRECTORY "/pairing.cfg"
#define PAIRING_URL_PREFIX "3gent://pair?"
#define PAIRING_PAYLOAD_VERSION 1
#define PAIRING_LINE_CAPACITY 160

static void set_error(char *error, size_t capacity, const char *message)
{
    if (error != NULL && capacity > 0) {
        snprintf(error, capacity, "%s", message);
    }
}

static bool copy_bounded(
    char *destination,
    size_t capacity,
    const char *value,
    size_t length
)
{
    if (length + 1 > capacity) {
        return false;
    }
    memcpy(destination, value, length);
    destination[length] = '\0';
    return true;
}

static bool parse_port(const char *value, size_t length, unsigned short *port)
{
    if (length == 0 || length > 5) {
        return false;
    }
    unsigned long parsed = 0;
    for (size_t index = 0; index < length; index++) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        parsed = parsed * 10u + (unsigned long)(value[index] - '0');
    }
    if (parsed == 0 || parsed > 65535u) {
        return false;
    }
    *port = (unsigned short)parsed;
    return true;
}

/* Uppercases and drops grouping characters so a typed code matches the QR. */
static bool copy_normalized_code(char *destination, size_t capacity, const char *value, size_t length)
{
    size_t written = 0;
    for (size_t index = 0; index < length; index++) {
        const unsigned char character = (unsigned char)value[index];
        if (!isalnum(character)) {
            continue;
        }
        if (written + 1 >= capacity) {
            return false;
        }
        destination[written++] = (char)toupper(character);
    }
    destination[written] = '\0';
    return written > 0;
}

bool pairing_parse_url(
    const char *text,
    PairingBootstrap *bootstrap,
    char *error,
    size_t error_capacity
)
{
    if (text == NULL || bootstrap == NULL
        || strncmp(text, PAIRING_URL_PREFIX, strlen(PAIRING_URL_PREFIX)) != 0) {
        set_error(error, error_capacity, "That is not a 3gent pairing code");
        return false;
    }

    memset(bootstrap, 0, sizeof(*bootstrap));
    snprintf(bootstrap->bridge_name, sizeof(bootstrap->bridge_name), "bridge");

    bool has_version = false;
    bool has_host = false;
    bool has_http_port = false;
    bool has_push_port = false;
    bool has_code = false;

    const char *cursor = text + strlen(PAIRING_URL_PREFIX);
    while (*cursor != '\0') {
        const char *equals = strchr(cursor, '=');
        if (equals == NULL) {
            break;
        }
        const char *end = strchr(equals + 1, '&');
        if (end == NULL) {
            end = equals + 1 + strlen(equals + 1);
        }
        const size_t key_length = (size_t)(equals - cursor);
        const char *value = equals + 1;
        const size_t value_length = (size_t)(end - value);

        if (key_length == 1) {
            switch (cursor[0]) {
                case 'v': {
                    unsigned short parsed = 0;
                    has_version = parse_port(value, value_length, &parsed)
                        && parsed == PAIRING_PAYLOAD_VERSION;
                    if (!has_version) {
                        set_error(
                            error,
                            error_capacity,
                            "This pairing code needs a newer 3gent build"
                        );
                        return false;
                    }
                    break;
                }
                case 'h':
                    has_host = copy_bounded(
                        bootstrap->host,
                        sizeof(bootstrap->host),
                        value,
                        value_length
                    );
                    break;
                case 'p':
                    has_http_port = parse_port(value, value_length, &bootstrap->http_port);
                    break;
                case 'q':
                    has_push_port = parse_port(value, value_length, &bootstrap->push_port);
                    break;
                case 'c':
                    has_code = copy_normalized_code(
                        bootstrap->code,
                        sizeof(bootstrap->code),
                        value,
                        value_length
                    );
                    break;
                case 'n':
                    copy_bounded(
                        bootstrap->bridge_name,
                        sizeof(bootstrap->bridge_name),
                        value,
                        value_length
                    );
                    break;
                default:
                    break;
            }
        }

        cursor = *end == '\0' ? end : end + 1;
    }

    if (!has_version || !has_host || !has_http_port || !has_push_port || !has_code) {
        set_error(error, error_capacity, "That pairing code is missing information");
        return false;
    }
    return true;
}

bool pairing_parse_manual(
    const char *text,
    PairingBootstrap *bootstrap,
    char *error,
    size_t error_capacity
)
{
    if (text == NULL || bootstrap == NULL) {
        set_error(error, error_capacity, "Enter the four values printed by the bridge");
        return false;
    }
    if (strncmp(text, PAIRING_URL_PREFIX, strlen(PAIRING_URL_PREFIX)) == 0) {
        return pairing_parse_url(text, bootstrap, error, error_capacity);
    }

    memset(bootstrap, 0, sizeof(*bootstrap));
    snprintf(bootstrap->bridge_name, sizeof(bootstrap->bridge_name), "bridge");

    const char *fields[4] = { NULL, NULL, NULL, NULL };
    size_t lengths[4] = { 0, 0, 0, 0 };
    size_t found = 0;
    const char *cursor = text;
    while (*cursor != '\0' && found < 4) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ':' || *cursor == ',') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t'
            && *cursor != ':' && *cursor != ',') {
            cursor++;
        }
        fields[found] = start;
        lengths[found] = (size_t)(cursor - start);
        found++;
    }

    if (found < 4) {
        set_error(
            error,
            error_capacity,
            "Enter host, port, push port and code, separated by spaces"
        );
        return false;
    }
    if (!copy_bounded(bootstrap->host, sizeof(bootstrap->host), fields[0], lengths[0])) {
        set_error(error, error_capacity, "That address is too long");
        return false;
    }
    if (!parse_port(fields[1], lengths[1], &bootstrap->http_port)
        || !parse_port(fields[2], lengths[2], &bootstrap->push_port)) {
        set_error(error, error_capacity, "Both ports must be numbers from 1 to 65535");
        return false;
    }
    if (!copy_normalized_code(
            bootstrap->code,
            sizeof(bootstrap->code),
            fields[3],
            lengths[3]
        )) {
        set_error(error, error_capacity, "That pairing code is not readable");
        return false;
    }
    return true;
}

const char *pairing_storage_path(void)
{
    return PAIRING_FILE;
}

static bool read_field(
    const char *line,
    const char *key,
    char *destination,
    size_t capacity
)
{
    const size_t key_length = strlen(key);
    if (strncmp(line, key, key_length) != 0 || line[key_length] != '=') {
        return false;
    }
    snprintf(destination, capacity, "%s", line + key_length + 1);
    return true;
}

bool pairing_load(PairingRecord *record)
{
    if (record == NULL) {
        return false;
    }
    memset(record, 0, sizeof(*record));

    FILE *file = fopen(PAIRING_FILE, "r");
    if (file == NULL) {
        return false;
    }

    char line[PAIRING_LINE_CAPACITY];
    char version[8] = "";
    char http_port[8] = "";
    char push_port[8] = "";
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0) {
            continue;
        }
        (void)(read_field(line, "version", version, sizeof(version))
            || read_field(line, "host", record->host, sizeof(record->host))
            || read_field(line, "http_port", http_port, sizeof(http_port))
            || read_field(line, "push_port", push_port, sizeof(push_port))
            || read_field(line, "bridge", record->bridge_name, sizeof(record->bridge_name))
            || read_field(line, "device_id", record->device_id, sizeof(record->device_id))
            || read_field(line, "token", record->token, sizeof(record->token))
            || read_field(line, "paired_at", record->paired_at, sizeof(record->paired_at)));
    }
    fclose(file);

    if (strcmp(version, "1") != 0
        || record->host[0] == '\0'
        || record->token[0] == '\0'
        || !parse_port(http_port, strlen(http_port), &record->http_port)
        || !parse_port(push_port, strlen(push_port), &record->push_port)) {
        memset(record, 0, sizeof(*record));
        return false;
    }
    if (record->bridge_name[0] == '\0') {
        snprintf(record->bridge_name, sizeof(record->bridge_name), "bridge");
    }
    record->valid = true;
    return true;
}

bool pairing_save(
    const PairingRecord *record,
    char *error,
    size_t error_capacity
)
{
    if (record == NULL || !record->valid) {
        set_error(error, error_capacity, "there is no pairing to save");
        return false;
    }
    mkdir("sdmc:/3ds", 0777);
    mkdir(PAIRING_DIRECTORY, 0777);

    FILE *file = fopen(PAIRING_FILE, "w");
    if (file == NULL) {
        set_error(error, error_capacity, "could not write to the SD card");
        return false;
    }
    const int written = fprintf(
        file,
        "version=1\n"
        "host=%s\n"
        "http_port=%u\n"
        "push_port=%u\n"
        "bridge=%s\n"
        "device_id=%s\n"
        "token=%s\n"
        "paired_at=%s\n",
        record->host,
        (unsigned int)record->http_port,
        (unsigned int)record->push_port,
        record->bridge_name,
        record->device_id,
        record->token,
        record->paired_at
    );
    const bool flushed = fclose(file) == 0;
    if (written < 0 || !flushed) {
        set_error(error, error_capacity, "the pairing file was not written completely");
        return false;
    }
    return true;
}

bool pairing_forget(char *error, size_t error_capacity)
{
    if (remove(PAIRING_FILE) == 0) {
        return true;
    }
    /* Nothing to remove is the outcome the caller asked for. */
    FILE *file = fopen(PAIRING_FILE, "r");
    if (file == NULL) {
        return true;
    }
    fclose(file);
    set_error(error, error_capacity, "could not remove the saved pairing");
    return false;
}
