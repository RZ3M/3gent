#ifndef THREEGENT_PAIRING_H
#define THREEGENT_PAIRING_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Pairing bootstrap parsing and on-SD credential storage (D-010, ADR-0001).
 *
 * A scanned or typed bootstrap carries an endpoint plus a short-lived one-time
 * code. It is never a credential. `main.c` exchanges it for a device token over
 * `POST /v1/pair`, and only the result is written to the SD card.
 *
 * Nothing here touches the network or the screen.
 */

#define PAIRING_HOST_CAPACITY 48
#define PAIRING_CODE_CAPACITY 24
#define PAIRING_BRIDGE_NAME_CAPACITY 24
#define PAIRING_DEVICE_ID_CAPACITY 40
#define PAIRING_TOKEN_CAPACITY 96
#define PAIRING_TIMESTAMP_CAPACITY 32

/* The short-lived material a QR code or a typed code carries. */
typedef struct {
    char host[PAIRING_HOST_CAPACITY];
    unsigned short http_port;
    unsigned short push_port;
    char code[PAIRING_CODE_CAPACITY];
    char bridge_name[PAIRING_BRIDGE_NAME_CAPACITY];
} PairingBootstrap;

/* The revocable credential kept on the SD card between runs. */
typedef struct {
    bool valid;
    char host[PAIRING_HOST_CAPACITY];
    unsigned short http_port;
    unsigned short push_port;
    char bridge_name[PAIRING_BRIDGE_NAME_CAPACITY];
    char device_id[PAIRING_DEVICE_ID_CAPACITY];
    char token[PAIRING_TOKEN_CAPACITY];
    char paired_at[PAIRING_TIMESTAMP_CAPACITY];
} PairingRecord;

/*
 * Parses `3gent://pair?v=1&h=..&p=..&q=..&c=..&n=..`. Rejects an unknown
 * payload version rather than guessing at the field meanings.
 */
bool pairing_parse_url(
    const char *text,
    PairingBootstrap *bootstrap,
    char *error,
    size_t error_capacity
);

/*
 * Parses the typed fallback `host httpPort pushPort CODE`. Separators may be
 * spaces, colons or commas, and the code may be grouped with dashes. A full
 * `3gent://` URL is also accepted here, so a user who can paste one does not
 * have to care which entry point they picked.
 */
bool pairing_parse_manual(
    const char *text,
    PairingBootstrap *bootstrap,
    char *error,
    size_t error_capacity
);

/* Absolute SD path of the stored credential, for display and diagnostics. */
const char *pairing_storage_path(void);

/* Reads the saved pairing. A missing or malformed file is not an error. */
bool pairing_load(PairingRecord *record);

bool pairing_save(
    const PairingRecord *record,
    char *error,
    size_t error_capacity
);

/* Deletes the stored credential. Revocation on the bridge is separate. */
bool pairing_forget(char *error, size_t error_capacity);

#endif
