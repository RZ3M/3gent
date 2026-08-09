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
 *
 * It also owns touch geometry. `ui_hit_test` resolves a bottom-screen point to
 * the same semantic action the renderer drew there, so a target can never drift
 * away from the thing it looks like.
 */

typedef enum {
    UI_SCREEN_BOOT = 0,
    UI_SCREEN_HOME,
    UI_SCREEN_PAIRING,
    UI_SCREEN_SESSIONS,
    UI_SCREEN_MAIN,
    UI_SCREEN_PHOTO,
} UiScreen;

/* Phases of the pairing screen, in the order the user meets them. */
typedef enum {
    UI_PAIRING_AIMING = 0,
    UI_PAIRING_DECODED,
    UI_PAIRING_EXCHANGING,
    UI_PAIRING_SUCCEEDED,
    UI_PAIRING_FAILED,
} UiPairingPhase;

/* What a task is doing, as far as the handheld last heard. */
typedef enum {
    UI_TASK_IDLE = 0,
    UI_TASK_WORKING,
    UI_TASK_ATTENTION, /* blocked on the user: an approval is waiting */
    UI_TASK_FAILED,
    UI_TASK_UNKNOWN,
} UiTaskState;

typedef struct {
    const char *label;
    UiTaskState state;
    /* Output arrived while the user was looking at a different task. */
    bool unread;
} UiTask;

#define UI_PHOTO_PROGRESS_NONE 0xFFFFFFFFu

/*
 * Semantic result of touching the bottom screen. Every action a finger can
 * reach is also on a button, so `main.c` folds a hit back into the button it
 * stands for and keeps one handler per action.
 */
typedef enum {
    UI_HIT_NONE = 0,
    UI_HIT_PRIMARY,       /* A */
    UI_HIT_SECONDARY,     /* X */
    UI_HIT_TERTIARY,      /* Y */
    UI_HIT_BACK,          /* B */
    UI_HIT_PHOTO,         /* L */
    UI_HIT_TALK,          /* hold R; held for as long as the finger stays down */
    UI_HIT_TASK,          /* switch to `index` in the task rail */
    UI_HIT_TASK_LIST,     /* open the task manager */
    UI_HIT_MENU_ROW,      /* choose row `index` of the on-screen list */
    UI_HIT_SCROLL_BACK,   /* one page further back through the response */
    UI_HIT_SCROLL_FORWARD,
    UI_HIT_SCROLL_LATEST,
} UiHitKind;

typedef struct {
    UiHitKind kind;
    size_t index;
} UiHit;

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

    /*
     * Tasks. `task_active` is the one being read; `task_selected` is the one
     * highlighted in the manager. They are the same index while the manager is
     * open on the current task, and differ as soon as the user moves.
     */
    const UiTask *tasks;
    size_t task_count;
    size_t task_selected;
    size_t task_active;
    bool task_active_valid;
    bool tasks_loading;
    bool tasks_retryable;
    const char *tasks_status;

    /* Photo review */
    const char *photo_caption;
    unsigned int photo_progress_percent;

    /* Start screen */
    const char *const *menu_labels;
    const char *const *menu_hints;
    const bool *menu_enabled;
    size_t menu_count;
    size_t menu_selected;
    bool paired;
    const char *paired_bridge;
    const char *paired_endpoint;
    const char *paired_since;

    /* Pairing */
    UiPairingPhase pairing_phase;
    bool pairing_preview_ready;
    const char *pairing_message;
    const char *pairing_bridge;
    unsigned int pairing_frames_examined;

    /* Touch, for drawing the pressed target the finger is on. */
    bool touch_down;
    unsigned int touch_x;
    unsigned int touch_y;
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

/* Lines of response text visible at once, for page-sized scroll steps. */
size_t ui_page_lines(const UiModel *model);

/*
 * Resolves a bottom-screen point to the action drawn there. Pure: it reads the
 * same model the frame was drawn from and shares the renderer's layout, so it
 * cannot report a target the user cannot see.
 */
UiHit ui_hit_test(const UiModel *model, unsigned int x, unsigned int y);

/* Uploads an RGB565 image for UI_SCREEN_PHOTO. */
bool ui_photo_preview_set(
    const u8 *rgb565,
    unsigned int width,
    unsigned int height
);
void ui_photo_preview_clear(void);

#endif
