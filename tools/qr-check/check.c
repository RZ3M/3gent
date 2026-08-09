/*
 * Decodes a QR matrix with the vendored quirc build that ships on the 3DS.
 *
 * It reads `#`/`.` rows on stdin (see `bridge/src/qr-matrix-cli.ts`), renders
 * them into a grayscale image at a chosen module scale with a quiet zone, and
 * prints the decoded payload. This is how the bridge's encoder and the
 * handheld's decoder are proved compatible without a hardware run: the same
 * source file that decodes camera frames on the device decodes the bridge's
 * output here.
 *
 *   make -C tools/qr-check run
 */
#include "quirc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODULES 200
#define QUIET_MODULES 4

static char matrix[MAX_MODULES][MAX_MODULES + 2];
static int matrix_size;

static int read_matrix(void)
{
    char line[MAX_MODULES + 4];
    matrix_size = 0;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        if (length == 0) {
            continue;
        }
        if (matrix_size >= MAX_MODULES || length > MAX_MODULES) {
            fprintf(stderr, "matrix is larger than %d modules\n", MAX_MODULES);
            return -1;
        }
        if (matrix_size > 0 && length != strlen(matrix[0])) {
            fprintf(stderr, "matrix rows have inconsistent widths\n");
            return -1;
        }
        memcpy(matrix[matrix_size], line, length + 1);
        matrix_size++;
    }
    if (matrix_size == 0 || (size_t)matrix_size != strlen(matrix[0])) {
        fprintf(stderr, "matrix is empty or not square\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const int scale = argc > 1 ? atoi(argv[1]) : 4;
    if (scale < 1 || scale > 32) {
        fprintf(stderr, "scale must be 1..32\n");
        return 2;
    }
    if (read_matrix() != 0) {
        return 2;
    }

    const int side = (matrix_size + 2 * QUIET_MODULES) * scale;
    struct quirc *decoder = quirc_new();
    if (decoder == NULL || quirc_resize(decoder, side, side) < 0) {
        fprintf(stderr, "could not allocate a %dx%d decoder\n", side, side);
        return 1;
    }

    int width = 0;
    int height = 0;
    uint8_t *image = quirc_begin(decoder, &width, &height);
    memset(image, 0xFF, (size_t)width * (size_t)height);
    for (int y = 0; y < matrix_size; y++) {
        for (int x = 0; x < matrix_size; x++) {
            if (matrix[y][x] != '#') {
                continue;
            }
            for (int dy = 0; dy < scale; dy++) {
                const int row = (y + QUIET_MODULES) * scale + dy;
                uint8_t *line = image + (size_t)row * (size_t)width
                    + (size_t)((x + QUIET_MODULES) * scale);
                memset(line, 0x00, (size_t)scale);
            }
        }
    }
    quirc_end(decoder);

    const int count = quirc_count(decoder);
    if (count < 1) {
        fprintf(stderr, "quirc found no QR code in a %dx%d image\n", side, side);
        quirc_destroy(decoder);
        return 1;
    }

    struct quirc_code *code = malloc(sizeof(*code));
    struct quirc_data *data = malloc(sizeof(*data));
    if (code == NULL || data == NULL) {
        fprintf(stderr, "out of memory\n");
        free(code);
        free(data);
        quirc_destroy(decoder);
        return 1;
    }

    int status = 1;
    quirc_extract(decoder, 0, code);
    const quirc_decode_error_t error = quirc_decode(code, data);
    if (error != QUIRC_SUCCESS) {
        fprintf(stderr, "quirc_decode failed: %s\n", quirc_strerror(error));
    } else {
        printf("version=%d ecc_level=%d mask=%d bytes=%d\n",
            data->version, data->ecc_level, data->mask, data->payload_len);
        printf("%.*s\n", data->payload_len, (const char *)data->payload);
        status = 0;
    }

    free(code);
    free(data);
    quirc_destroy(decoder);
    return status;
}
