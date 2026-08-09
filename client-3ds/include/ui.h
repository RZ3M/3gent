#ifndef THREEGENT_UI_H
#define THREEGENT_UI_H

#include <3ds.h>

#include <stdbool.h>
#include <stddef.h>

/*
 * The presentation layer. It owns citro2d, the theme, text measurement, and
 * every pixel drawn on both screens. It never touches the network, the
 * microphone, or the camera: `main.c` fills a `UiModel` each frame and the
 * renderer reads it.
 */

typedef enum {
    UI_SCREEN_BOOT = 0,
    UI_SCREEN_SESSIONS,
    UI_SCREEN_MAIN,
    UI_SCREEN_PHOTO,
} UiScreen;

#define UI_PHOTO_PROGRESS_NONE 0xFFFFFFFFu

typedef struct {
    UiScreen screen;

    /* Identity and transport */
    const char *version;
    const char *session_label;
    const char *server_host;
    unsigned int server_port;
    bool network_ready;
    bool audio_warm;
    bool microphone_ready;
    const char *link_state;
    unsigned int event_cursor;

    /* Agent and phase */
    const char *agent_state;
    const char *view_state;
    const char *detail;
    const char *detail_secondary;
    bool turn_active;

    /* Read surface */
    const char *prompt;
    const char *response;
    size_t scroll_lines;

    /* Attention cards */
    bool approval_pending;
    const char *approval_summary;
    bool transcript_ready;
    const char *transcript;
    bool photo_pending;

    /* Voice capture */
    bool recording;
    unsigned int record_ms;
    unsigned int record_max_ms;
    unsigned int record_level_percent;

    /* Turn diff summary */
    bool diff_known;
    unsigned int diff_files;
    unsigned int diff_additions;
    unsigned int diff_deletions;

    /* Task chooser */
    const char *const *session_labels;
    size_t session_count;
    size_t session_selected;
    bool sessions_loading;
    bool sessions_retryable;
    const char *sessions_status;

    /* Photo review */
    const char *photo_caption;
    unsigned int photo_progress_percent;
} UiModel;

bool ui_initialize(char *error, size_t error_capacity);
void ui_shutdown(void);

/* Draws one complete frame on both screens and presents it. */
void ui_render(const UiModel *model);

/*
 * Largest `scroll_lines` value the response body can accept for this model.
 * Uses the same wrapping and layout the renderer uses, so scrolling can never
 * disagree with what is on screen.
 */
size_t ui_max_scroll(const UiModel *model);

/* Uploads an RGB565 image for UI_SCREEN_PHOTO. */
bool ui_photo_preview_set(
    const u8 *rgb565,
    unsigned int width,
    unsigned int height
);
void ui_photo_preview_clear(void);

#endif
