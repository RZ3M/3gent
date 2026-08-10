#include "app_config.h"
#include "camera_capture.h"
#include "microphone.h"
#include "network.h"
#include "pairing.h"
#include "qr_scanner.h"
#include "ui.h"

#include <3ds.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCROLL_REPEAT_DELAY_FRAMES 18
#define SCROLL_REPEAT_INTERVAL_FRAMES 4
#define MIC_STREAM_BUFFER_CAPACITY 8192
#define MIC_STREAM_SEND_THRESHOLD 1024
#define MAX_SESSION_CHOICES 6
#define SESSION_LABEL_CAPACITY 72

typedef enum {
    PUSH_COMMAND_NONE = 0,
    PUSH_COMMAND_TEXT_CAPTURE,
    PUSH_COMMAND_APPROVAL,
    PUSH_COMMAND_INTERRUPT,
} PushCommandKind;

static char prompt[THREEGENT_PROMPT_CAPACITY];
static char response[THREEGENT_RESPONSE_CAPACITY];
static char network_detail[160];
static char microphone_detail[160];
static char agent_state[24] = "connecting";
static char pending_approval_id[65];
static char pending_approval_summary[96];
static char view_state[32] = "Ready";
static char event_detail[96];
static char camera_detail[160];
static u8 microphone_stream_buffer[MIC_STREAM_BUFFER_CAPACITY];
static bool network_ready;
static bool recording_session_active;
static bool microphone_stop_requested;
static bool microphone_maximum_reached;
static bool microphone_network_finish_requested;
static bool microphone_link_ready;
static size_t microphone_stream_size;
static u64 microphone_link_started_ms;
static u64 microphone_finish_started_ms;
static unsigned int microphone_link_latency_ms;
static size_t response_scroll_lines;
static int scroll_repeat_direction;
static u32 scroll_repeat_frames;
static unsigned int event_cursor;
static bool turn_active;
static PushCommandKind push_command_kind;
static u64 push_command_started_ms;
static char current_session_id[65];
static char current_session_label[SESSION_LABEL_CAPACITY];
static char audio_capture_path[128];
static char session_ids[MAX_SESSION_CHOICES][65];
static char session_labels[MAX_SESSION_CHOICES][SESSION_LABEL_CAPACITY];
static UiTask tasks[MAX_SESSION_CHOICES];
/*
 * The last event sequence the user has actually seen for each task. The bridge
 * reports every task's `lastSequence`, so a task that has moved past what we
 * last showed is carrying output the user has not read.
 */
static unsigned int task_seen_sequence[MAX_SESSION_CHOICES];
static size_t task_count;
static size_t task_selected;
static size_t task_active;
static bool task_active_valid;
static bool tasks_loading;
static bool tasks_retryable;
static char task_status_line[160];
static bool task_refresh_active;
static u64 task_refresh_checked_ms;
static char pending_transcript[THREEGENT_PROMPT_CAPACITY];
static bool transcript_ready;
static bool photo_pending;
static char photo_capture_path[128];
static const char *photo_caption = "";
static unsigned int photo_progress_percent = UI_PHOTO_PROGRESS_NONE;
static bool diff_known;
static unsigned int diff_files;
static unsigned int diff_additions;
static unsigned int diff_deletions;
static UiScreen current_screen = UI_SCREEN_BOOT;

/* Touch, resolved against the frame the user is actually looking at. */
static bool touch_is_down;
static unsigned int touch_point_x;
static unsigned int touch_point_y;
static UiHit touch_armed;
static UiHit touch_released;

/*
 * An approval must not be answerable by a keypress that was already on its way
 * to something else, so A is ignored for a moment after the request appears.
 * The product rule is that approvals are hard to trigger by accident; putting
 * approve on A satisfies the button grammar, and this preserves the rule.
 */
#define APPROVAL_ARMING_MS 450
static u64 approval_arrived_ms;

/*
 * The endpoint is runtime state, not a build constant. A paired machine
 * supplies it; the compile-time SERVER_HOST remains as the development
 * fallback so an unpaired build still behaves the way it did before.
 */
static PairingRecord pairing_record;
static char active_host[PAIRING_HOST_CAPACITY] = THREEGENT_SERVER_HOST;
static unsigned short active_port = THREEGENT_SERVER_PORT;
static unsigned short active_push_port = THREEGENT_PUSH_PORT;
static char pairing_message[160];
static char pairing_bridge_line[96];
static char paired_endpoint_line[80];
static char paired_since_line[64];
static UiPairingPhase pairing_phase;
static bool pairing_preview_ready;

/* Start-screen menu. The order is fixed so HomeAction can index it directly. */
typedef enum {
    HOME_CONNECT = 0,
    HOME_PAIR_QR,
    HOME_PAIR_MANUAL,
    HOME_FORGET,
    HOME_EXIT,
    HOME_MENU_COUNT,
} HomeAction;

static char home_connect_hint[96];
static const char *home_menu_labels[HOME_MENU_COUNT] = {
    "Connect",
    "Pair with a QR code",
    "Enter a pairing code",
    "Forget this machine",
    "Exit",
};
static const char *home_menu_hints[HOME_MENU_COUNT] = {
    "",
    "Scan the code your bridge prints",
    "Type the address and code by hand",
    "Delete the device key saved on the SD card",
    "Close 3gent",
};
static bool home_menu_enabled[HOME_MENU_COUNT] = {
    true, true, true, false, true,
};
static size_t menu_selected;

static bool restart_push_link(void);

static void set_view_state(const char *state)
{
    snprintf(view_state, sizeof(view_state), "%s", state);
}

/* ----------------------------------------------------------- read surface -- */

/* Any replacement of the response jumps back to the newest line. */
static void clear_response(void)
{
    response[0] = '\0';
    response_scroll_lines = 0;
}

static void set_response(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(response, sizeof(response), format, arguments);
    va_end(arguments);
    response_scroll_lines = 0;
}

/*
 * Keeps the newest output rather than the oldest. Replaying a long task from
 * the start used to fill the buffer and then silently discard everything after
 * it, which is exactly backwards for a screen whose job is to show what the
 * agent is doing now. Dropping whole lines from the front keeps the buffer a
 * readable tail window and keeps memory bounded.
 */
static void append_response(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    const size_t incoming = strlen(text);
    size_t used = strlen(response);

    if (incoming + 1 >= sizeof(response)) {
        /* One oversized chunk: keep its tail, which is the most recent part. */
        const size_t kept = sizeof(response) - 1;
        memmove(response, text + (incoming - kept), kept);
        response[kept] = '\0';
        response_scroll_lines = 0;
        return;
    }

    if (used + incoming + 1 > sizeof(response)) {
        const size_t needed = used + incoming + 1 - sizeof(response);
        size_t drop = needed;
        /* Prefer cutting at a line break so the top of the view is not a stub. */
        while (drop < used && response[drop] != '\n') {
            drop++;
        }
        if (drop < used) {
            drop++;
        } else {
            drop = needed;
        }
        memmove(response, response + drop, used - drop + 1);
        used -= drop;
    }

    memcpy(response + used, text, incoming + 1);
    response_scroll_lines = 0;
}

/* --------------------------------------------------------------- rendering -- */

static const char *primary_detail(void)
{
    static char scratch[160];

    if (recording_session_active) {
        snprintf(
            scratch,
            sizeof(scratch),
            "PCM %u ms | MICU %s | no new data %u ms",
            microphone_duration_ms(),
            microphone_service_is_sampling() ? "sampling" : "stopped",
            microphone_stall_ms()
        );
        return scratch;
    }
    const bool camera_screen = current_screen == UI_SCREEN_PHOTO
        || current_screen == UI_SCREEN_PAIRING;
    if (camera_screen && camera_detail[0] != '\0') {
        return camera_detail;
    }
    if (network_detail[0] != '\0') {
        return network_detail;
    }
    if (microphone_detail[0] != '\0') {
        return microphone_detail;
    }
    return camera_detail;
}

static const char *secondary_detail(void)
{
    if (recording_session_active && microphone_detail[0] != '\0') {
        return microphone_detail;
    }
    return event_detail;
}

static void build_model(UiModel *model)
{
    memset(model, 0, sizeof(*model));

    model->screen = current_screen;
    model->version = THREEGENT_APP_VERSION;
    model->session_label = current_session_label;
    model->server_host = active_host;
    model->server_port = (unsigned int)active_port;
    model->network_ready = network_ready;
    model->audio_warm = network_warm_connection_count() > 0;
    model->microphone_ready = microphone_is_ready();
    model->link_state = network_push_state();
    model->event_cursor = event_cursor;

    model->agent_state = agent_state;
    model->view_state = view_state;
    model->detail = primary_detail();
    model->detail_secondary = secondary_detail();
    model->turn_active = turn_active;

    model->prompt = prompt;
    model->response = response;
    model->scroll_lines = response_scroll_lines;

    model->approval_pending = pending_approval_id[0] != '\0';
    model->approval_summary = pending_approval_summary;
    model->transcript_ready = transcript_ready;
    model->transcript = pending_transcript;
    model->photo_pending = photo_pending;

    model->recording = recording_session_active;
    model->record_ms = microphone_wall_duration_ms();
    model->record_max_ms = THREEGENT_MIC_MAX_SECONDS * 1000u;
    model->record_level_percent = microphone_level_percent();

    model->diff_known = diff_known;
    model->diff_files = diff_files;
    model->diff_additions = diff_additions;
    model->diff_deletions = diff_deletions;

    model->tasks = tasks;
    model->task_count = task_count;
    model->task_selected = task_selected;
    model->task_active = task_active;
    model->task_active_valid = task_active_valid && task_active < task_count;
    model->tasks_loading = tasks_loading;
    model->tasks_retryable = tasks_retryable;
    model->tasks_status = task_status_line;

    model->photo_caption = photo_caption;
    model->photo_progress_percent = photo_progress_percent;

    model->menu_labels = home_menu_labels;
    model->menu_hints = home_menu_hints;
    model->menu_enabled = home_menu_enabled;
    model->menu_count = HOME_MENU_COUNT;
    model->menu_selected = menu_selected;
    model->paired = pairing_record.valid;
    model->paired_bridge = pairing_record.bridge_name;
    model->paired_endpoint = paired_endpoint_line;
    model->paired_since = paired_since_line;

    model->pairing_phase = pairing_phase;
    model->pairing_preview_ready = pairing_preview_ready;
    model->pairing_message = pairing_message;
    model->pairing_bridge = pairing_bridge_line;
    model->pairing_frames_examined = qr_scanner_frames_examined();

    model->touch_down = touch_is_down;
    model->touch_x = touch_point_x;
    model->touch_y = touch_point_y;
}

/*
 * Touch is read once per frame and folded into the same handlers the physical
 * keys use, so there is exactly one implementation of every action. A target is
 * armed on contact and fires on release over the same target, which makes a
 * mis-aimed stylus recoverable by sliding off before lifting.
 */
static void update_touch(u32 keys_down, u32 keys_held, u32 keys_up)
{
    /*
     * The touch position is only meaningful while contact is held; after the
     * lift it reads as the origin, which is a live target on some screens. So
     * the last held point is retained and the release is judged against that.
     */
    if ((keys_held & KEY_TOUCH) != 0) {
        touchPosition touch;
        hidTouchRead(&touch);
        touch_point_x = touch.px;
        touch_point_y = touch.py;
        touch_is_down = true;
    } else {
        touch_is_down = false;
    }

    if ((keys_down & KEY_TOUCH) != 0) {
        UiModel model;
        build_model(&model);
        touch_armed = ui_hit_test(&model, touch_point_x, touch_point_y);
        touch_released.kind = UI_HIT_NONE;
        return;
    }

    if ((keys_up & KEY_TOUCH) != 0) {
        UiModel model;
        build_model(&model);
        const UiHit lifted = ui_hit_test(&model, touch_point_x, touch_point_y);
        touch_released = (lifted.kind == touch_armed.kind
                && lifted.index == touch_armed.index)
            ? lifted
            : (UiHit){ UI_HIT_NONE, 0 };
        touch_armed.kind = UI_HIT_NONE;
        return;
    }

    touch_released.kind = UI_HIT_NONE;
}

/* True while the user is holding the on-screen push-to-talk panel. */
static bool touch_talk_held(u32 keys_held)
{
    return (keys_held & KEY_TOUCH) != 0 && touch_armed.kind == UI_HIT_TALK;
}

/* Folds a completed tap into the key it stands for. */
static u32 touch_virtual_key(void)
{
    switch (touch_released.kind) {
        case UI_HIT_PRIMARY:   return KEY_A;
        case UI_HIT_SECONDARY: return KEY_X;
        case UI_HIT_TERTIARY:  return KEY_Y;
        case UI_HIT_BACK:      return KEY_B;
        case UI_HIT_PHOTO:     return KEY_L;
        default:               return 0;
    }
}

static void render_frame(void)
{
    UiModel model;
    build_model(&model);
    ui_render(&model);
}

/* Handed to the network module so its blocking waits keep the screen alive. */
static void network_wait_redraw(void *user_data)
{
    (void)user_data;
    render_frame();
}

static size_t get_max_scroll(void)
{
    UiModel model;
    build_model(&model);
    return ui_max_scroll(&model);
}

/* ------------------------------------------------------------ JSON reading -- */

static const char *find_json_value(const char *json, const char *key)
{
    char pattern[48];
    int pattern_size = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    if (pattern_size < 0 || (size_t)pattern_size >= sizeof(pattern)) {
        return NULL;
    }

    const char *value = strstr(json, pattern);
    if (value == NULL) {
        return NULL;
    }
    value += pattern_size;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    return value;
}

static bool json_read_unsigned(
    const char *json,
    const char *key,
    unsigned int *value
)
{
    const char *encoded = find_json_value(json, key);
    if (encoded == NULL || *encoded < '0' || *encoded > '9') {
        return false;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(encoded, &end, 10);
    if (end == encoded) {
        return false;
    }
    *value = (unsigned int)parsed;
    return true;
}

static bool json_read_string(
    const char *json,
    const char *key,
    char *value,
    size_t value_capacity
)
{
    const char *encoded = find_json_value(json, key);
    if (encoded == NULL || *encoded != '"' || value_capacity == 0) {
        return false;
    }

    encoded++;
    size_t written = 0;
    while (*encoded != '\0' && *encoded != '"') {
        char decoded = *encoded++;
        if (decoded == '\\') {
            char escape = *encoded++;
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    decoded = escape;
                    break;
                case 'b':
                    decoded = '\b';
                    break;
                case 'f':
                    decoded = '\f';
                    break;
                case 'n':
                    decoded = '\n';
                    break;
                case 'r':
                    decoded = '\r';
                    break;
                case 't':
                    decoded = '\t';
                    break;
                case 'u':
                    decoded = '?';
                    for (unsigned int digit = 0;
                         digit < 4 && *encoded != '\0';
                         digit++) {
                        encoded++;
                    }
                    break;
                default:
                    return false;
            }
        }

        if (written + 1 < value_capacity) {
            value[written++] = decoded;
        }
    }
    if (*encoded != '"') {
        return false;
    }
    value[written] = '\0';
    return true;
}

/* ---------------------------------------------------------- task selection -- */

static void set_task_status(const char *status)
{
    snprintf(task_status_line, sizeof(task_status_line), "%s", status != NULL ? status : "");
}

static bool wait_for_control_request(const char *status)
{
    tasks_loading = true;
    set_view_state(status);
    set_task_status(status);
    render_frame();

    while (aptMainLoop()) {
        hidScanInput();
        /* B is "back" everywhere, including out of a request that is hanging. */
        if ((hidKeysDown() & (KEY_B | KEY_START)) != 0) {
            network_control_cancel();
            tasks_loading = false;
            return false;
        }
        network_pump();
        NetworkOperationStatus operation = network_control_status();
        if (operation == NETWORK_OPERATION_SUCCEEDED) {
            tasks_loading = false;
            return true;
        }
        if (operation == NETWORK_OPERATION_FAILED) {
            snprintf(
                network_detail,
                sizeof(network_detail),
                "Session request failed: %.120s",
                network_control_error()
            );
            network_control_consume();
            tasks_loading = false;
            return false;
        }
        render_frame();
    }
    network_control_cancel();
    tasks_loading = false;
    return false;
}

static UiTaskState task_state_from_summary(
    const char *state,
    const char *pending_approval_id
)
{
    if (pending_approval_id[0] != '\0'
        || strcmp(state, "waiting_for_user") == 0) {
        return UI_TASK_ATTENTION;
    }
    if (strcmp(state, "working") == 0) {
        return UI_TASK_WORKING;
    }
    if (strcmp(state, "failed") == 0) {
        return UI_TASK_FAILED;
    }
    if (strcmp(state, "idle") == 0 || strcmp(state, "completed") == 0) {
        return UI_TASK_IDLE;
    }
    return UI_TASK_UNKNOWN;
}

/*
 * Parses a `/v1/sessions` body into the task list. It keeps the open task
 * selected by matching its session ID rather than its position, because the
 * bridge orders by recency and the list reshuffles underneath the user.
 */
static void apply_session_list(const char *body)
{
    char previous_active_id[sizeof(session_ids[0])] = "";
    if (task_active_valid && task_active < task_count) {
        snprintf(
            previous_active_id,
            sizeof(previous_active_id),
            "%.64s",
            session_ids[task_active]
        );
    }
    /* Seen-sequence is keyed by session ID, so remember it before reordering. */
    char previous_ids[MAX_SESSION_CHOICES][sizeof(session_ids[0])];
    unsigned int previous_seen[MAX_SESSION_CHOICES];
    const size_t previous_count = task_count < MAX_SESSION_CHOICES
        ? task_count
        : MAX_SESSION_CHOICES;
    for (size_t index = 0; index < previous_count; index++) {
        snprintf(previous_ids[index], sizeof(previous_ids[index]), "%.64s", session_ids[index]);
        previous_seen[index] = task_seen_sequence[index];
    }

    const char *cursor = strstr(body, "\"sessions\":[");
    task_count = 0;
    if (cursor != NULL) {
        while (task_count < MAX_SESSION_CHOICES) {
            cursor = strstr(cursor, "\"sessionId\":");
            if (cursor == NULL) {
                break;
            }
            if (!json_read_string(
                    cursor,
                    "sessionId",
                    session_ids[task_count],
                    sizeof(session_ids[task_count])
                ) || !json_read_string(
                    cursor,
                    "label",
                    session_labels[task_count],
                    sizeof(session_labels[task_count])
                )) {
                break;
            }

            char state[24] = "";
            char approval[65] = "";
            unsigned int last_sequence = 0;
            json_read_string(cursor, "state", state, sizeof(state));
            json_read_string(cursor, "pendingApprovalId", approval, sizeof(approval));
            json_read_unsigned(cursor, "lastSequence", &last_sequence);

            /*
             * A task the user is currently reading is never unread; anything
             * else is unread once the bridge has moved past what we last showed.
             * A task we have never seen starts read, so opening the app does not
             * claim that every existing task is new.
             */
            unsigned int seen = last_sequence;
            if (previous_active_id[0] == '\0'
                || strcmp(previous_active_id, session_ids[task_count]) != 0) {
                for (size_t index = 0; index < previous_count; index++) {
                    if (strcmp(previous_ids[index], session_ids[task_count]) == 0) {
                        seen = previous_seen[index];
                        break;
                    }
                }
            }

            tasks[task_count].label = session_labels[task_count];
            tasks[task_count].state = task_state_from_summary(state, approval);
            tasks[task_count].unread = last_sequence > seen;
            task_seen_sequence[task_count] = seen;
            task_count++;
            cursor += strlen("\"sessionId\":");
        }
    }

    task_active_valid = false;
    if (previous_active_id[0] != '\0') {
        for (size_t index = 0; index < task_count; index++) {
            if (strcmp(session_ids[index], previous_active_id) == 0) {
                task_active = index;
                task_active_valid = true;
                break;
            }
        }
    }
    /* `task_count` is the "start a new task" row, so it is a legal selection. */
    if (task_selected > task_count) {
        task_selected = task_count;
    }
}

static bool load_session_choices(void)
{
    if (!network_control_begin_get(
            active_host,
            active_port,
            "/v1/sessions?limit=6",
            network_detail,
            sizeof(network_detail)
        ) || !wait_for_control_request("Loading recent tasks...")) {
        return false;
    }
    if (network_control_http_status() != 200) {
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Session list returned HTTP %u",
            network_control_http_status()
        );
        network_control_consume();
        return false;
    }

    apply_session_list(network_control_response());
    network_control_consume();
    return true;
}

/*
 * A task the user is not looking at can start needing them at any moment, so
 * the list is re-read in the background while the agent loop runs. It never
 * competes for the single control slot: it only starts when nothing else holds
 * it, and any foreground request cancels it first.
 */
#define TASK_REFRESH_INTERVAL_MS 5000

static void cancel_task_refresh(void)
{
    if (task_refresh_active) {
        network_control_cancel();
        task_refresh_active = false;
    }
}

static void update_task_refresh(void)
{
    if (task_refresh_active) {
        const NetworkOperationStatus status = network_control_status();
        if (status == NETWORK_OPERATION_SUCCEEDED) {
            if (network_control_http_status() == 200) {
                apply_session_list(network_control_response());
            }
            network_control_consume();
            task_refresh_active = false;
            task_refresh_checked_ms = osGetTime();
        } else if (status == NETWORK_OPERATION_FAILED) {
            network_control_consume();
            task_refresh_active = false;
            task_refresh_checked_ms = osGetTime();
        }
        return;
    }

    if (!network_ready
        || recording_session_active
        || network_control_status() != NETWORK_OPERATION_IDLE) {
        return;
    }
    const u64 now = osGetTime();
    if (now - task_refresh_checked_ms < TASK_REFRESH_INTERVAL_MS) {
        return;
    }
    task_refresh_checked_ms = now;

    char error[96];
    task_refresh_active = network_control_begin_get(
        active_host,
        active_port,
        "/v1/sessions?limit=6",
        error,
        sizeof(error)
    );
}

static bool start_new_session(void)
{
    cancel_task_refresh();
    const char body[] = "{}";
    if (!network_control_begin_post(
            active_host,
            active_port,
            "/v1/sessions",
            "application/json",
            body,
            sizeof(body) - 1,
            network_detail,
            sizeof(network_detail)
        ) || !wait_for_control_request("Starting a new Codex task...")) {
        return false;
    }
    if (network_control_http_status() != 201
        && network_control_http_status() != 200) {
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Start task returned HTTP %u",
            network_control_http_status()
        );
        network_control_consume();
        return false;
    }
    bool parsed = json_read_string(
        network_control_response(),
        "sessionId",
        current_session_id,
        sizeof(current_session_id)
    );
    network_control_consume();
    if (!parsed) {
        snprintf(network_detail, sizeof(network_detail), "Start task response was malformed");
        return false;
    }
    snprintf(current_session_label, sizeof(current_session_label), "New Codex task");
    return true;
}

static bool resume_session(size_t selected)
{
    if (selected >= task_count) {
        return false;
    }
    cancel_task_refresh();
    char path[112];
    snprintf(path, sizeof(path), "/v1/sessions/%s/resume", session_ids[selected]);
    if (!network_control_begin_post(
            active_host,
            active_port,
            path,
            "application/json",
            NULL,
            0,
            network_detail,
            sizeof(network_detail)
        ) || !wait_for_control_request("Resuming selected task...")) {
        return false;
    }
    if (network_control_http_status() != 202
        && network_control_http_status() != 200) {
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Resume task returned HTTP %u",
            network_control_http_status()
        );
        network_control_consume();
        return false;
    }
    network_control_consume();
    snprintf(current_session_id, sizeof(current_session_id), "%s", session_ids[selected]);
    snprintf(current_session_label, sizeof(current_session_label), "%s", session_labels[selected]);
    return true;
}

/*
 * Points every per-task path and the pushed link at whatever `current_session_id`
 * now holds. Called after resuming or starting a task, whether that happened in
 * the manager or from the rail without leaving the agent loop.
 */
static void attach_to_current_session(void)
{
    snprintf(
        audio_capture_path,
        sizeof(audio_capture_path),
        "/v1/sessions/%s/captures/audio",
        current_session_id
    );
    snprintf(
        photo_capture_path,
        sizeof(photo_capture_path),
        "/v1/sessions/%s/captures/photo",
        current_session_id
    );

    /*
     * The task being left has been read up to the cursor we applied, so record
     * that before it is reset. Anything the bridge adds afterwards is what makes
     * the rail mark it unread.
     */
    if (task_active_valid && task_active < task_count) {
        task_seen_sequence[task_active] = event_cursor;
    }

    /*
     * Everything below belongs to the task being left. A transcript that was
     * never sent, a half-read response and an approval that is scoped to the
     * other session must not follow the user across.
     */
    event_cursor = 0;
    clear_response();
    prompt[0] = '\0';
    pending_transcript[0] = '\0';
    transcript_ready = false;
    pending_approval_id[0] = '\0';
    pending_approval_summary[0] = '\0';
    photo_pending = false;
    diff_known = false;
    turn_active = false;
    snprintf(agent_state, sizeof(agent_state), "connecting");

    task_active_valid = false;
    for (size_t index = 0; index < task_count; index++) {
        if (strcmp(session_ids[index], current_session_id) == 0) {
            task_active = index;
            task_selected = index;
            task_active_valid = true;
            tasks[index].unread = false;
            task_seen_sequence[index] = 0xFFFFFFFFu;
            break;
        }
    }

    network_push_stop();
    restart_push_link();
}

/* Opens task `index` in place. Returns false and says why if the bridge says no. */
static bool open_task(size_t index)
{
    if (index >= task_count) {
        return false;
    }
    if (task_active_valid && index == task_active) {
        return true;
    }
    network_push_stop();
    if (!resume_session(index)) {
        set_task_status(network_detail);
        set_view_state("Could not open that task");
        /* The previous task is still the live one; put its link back. */
        restart_push_link();
        return false;
    }
    attach_to_current_session();
    set_view_state("Opened task");
    return true;
}

static bool start_and_open_new_task(void)
{
    network_push_stop();
    if (!start_new_session()) {
        set_task_status(network_detail);
        restart_push_link();
        return false;
    }
    /* The new task is not in the list yet, so refresh before attaching. */
    load_session_choices();
    attach_to_current_session();
    set_view_state("New task started");
    return true;
}

typedef enum {
    TASKS_OPENED = 0,
    TASKS_BACK,
} TasksResult;

/*
 * The task manager. Rows are the targets, so touch and the D-pad reach the same
 * thing; A opens, X starts a new one, B goes back to the start screen.
 */
static TasksResult run_task_manager(void)
{
    current_screen = UI_SCREEN_SESSIONS;
    cancel_task_refresh();

    while (!load_session_choices()) {
        task_count = 0;
        tasks_retryable = true;
        bool retry = false;
        set_view_state("Task discovery failed");
        set_task_status(network_detail);
        render_frame();

        while (aptMainLoop()) {
            hidScanInput();
            const u32 keys_down = hidKeysDown();
            update_touch(keys_down, hidKeysHeld(), hidKeysUp());
            const u32 keys = keys_down | touch_virtual_key();
            if ((keys & (KEY_B | KEY_START)) != 0) {
                return TASKS_BACK;
            }
            if ((keys & (KEY_A | KEY_X)) != 0) {
                retry = true;
                break;
            }
            render_frame();
        }
        if (!retry) {
            return TASKS_BACK;
        }
    }

    tasks_retryable = false;
    set_view_state("Tasks");
    set_task_status("");

    while (aptMainLoop()) {
        hidScanInput();
        const u32 keys_down = hidKeysDown();
        update_touch(keys_down, hidKeysHeld(), hidKeysUp());
        u32 keys = keys_down | touch_virtual_key();
        network_pump();

        if (touch_released.kind == UI_HIT_MENU_ROW
            && touch_released.index < task_count) {
            task_selected = touch_released.index;
            keys |= KEY_A;
        }

        /*
         * The list is the tasks plus one more row: "start a new task". Walking
         * off the bottom of the tasks lands on it rather than wrapping, so the
         * D-pad and the stylus reach the same set of rows.
         */
        const size_t row_count = task_count + 1;

        if ((keys & (KEY_B | KEY_START)) != 0) {
            return TASKS_BACK;
        }
        if ((keys & (KEY_DUP | KEY_CPAD_UP)) != 0) {
            task_selected = task_selected == 0
                ? row_count - 1
                : task_selected - 1;
        }
        if ((keys & (KEY_DDOWN | KEY_CPAD_DOWN)) != 0) {
            task_selected = (task_selected + 1) % row_count;
        }
        if ((keys & KEY_A) != 0) {
            /* A on the last row means the same thing X means everywhere here. */
            if (task_selected >= task_count) {
                keys |= KEY_X;
            } else if (resume_session(task_selected)) {
                attach_to_current_session();
                return TASKS_OPENED;
            } else {
                set_task_status(network_detail);
            }
        }
        if ((keys & KEY_X) != 0) {
            if (start_new_session()) {
                load_session_choices();
                attach_to_current_session();
                return TASKS_OPENED;
            }
            set_task_status(network_detail);
        }
        render_frame();
    }
    return TASKS_BACK;
}

/* ---------------------------------------------------------------- pairing -- */

static void set_pairing_message(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(pairing_message, sizeof(pairing_message), format, arguments);
    va_end(arguments);
}

/*
 * Points every later request at `record`'s machine. Pooled sockets belong to
 * the previous endpoint, so they go first.
 */
static void apply_pairing(const PairingRecord *record)
{
    network_reset_connections();
    if (record != NULL && record->valid) {
        pairing_record = *record;
        snprintf(active_host, sizeof(active_host), "%s", record->host);
        active_port = record->http_port;
        active_push_port = record->push_port;
        network_set_device_token(record->token);
    } else {
        memset(&pairing_record, 0, sizeof(pairing_record));
        snprintf(active_host, sizeof(active_host), "%s", THREEGENT_SERVER_HOST);
        active_port = THREEGENT_SERVER_PORT;
        active_push_port = THREEGENT_PUSH_PORT;
        network_set_device_token(NULL);
    }

    snprintf(
        paired_endpoint_line,
        sizeof(paired_endpoint_line),
        "%s  port %u  push %u",
        active_host,
        (unsigned int)active_port,
        (unsigned int)active_push_port
    );
    if (pairing_record.valid) {
        snprintf(
            paired_since_line,
            sizeof(paired_since_line),
            "paired %.10s  key %s",
            pairing_record.paired_at[0] != '\0' ? pairing_record.paired_at : "recently",
            pairing_record.device_id
        );
        snprintf(
            home_connect_hint,
            sizeof(home_connect_hint),
            "%s at %s:%u",
            pairing_record.bridge_name,
            active_host,
            (unsigned int)active_port
        );
    } else {
        paired_since_line[0] = '\0';
        snprintf(
            home_connect_hint,
            sizeof(home_connect_hint),
            "development host %s:%u",
            active_host,
            (unsigned int)active_port
        );
    }
    home_menu_hints[HOME_CONNECT] = home_connect_hint;
    home_menu_enabled[HOME_FORGET] = pairing_record.valid;
}

/* The bridge lists this beside the device key, so make it identifiable. */
static const char *handheld_display_name(void)
{
    static char name[40];
    if (name[0] != '\0') {
        return name;
    }
    snprintf(name, sizeof(name), "Nintendo 3DS");
    if (R_FAILED(cfguInit())) {
        return name;
    }
    u8 model = 0;
    if (R_SUCCEEDED(CFGU_GetSystemModel(&model))) {
        static const char *const models[] = {
            "Old 3DS", "Old 3DS XL", "New 3DS", "2DS", "New 3DS XL", "New 2DS XL",
        };
        if (model < sizeof(models) / sizeof(models[0])) {
            snprintf(name, sizeof(name), "%s", models[model]);
        }
    }
    cfguExit();
    return name;
}

/* Renders the pairing screen while the exchange request is in flight. */
static bool pairing_wait_for_control(void)
{
    render_frame();
    while (aptMainLoop()) {
        hidScanInput();
        if ((hidKeysDown() & (KEY_B | KEY_START)) != 0) {
            network_control_cancel();
            set_pairing_message("Pairing was cancelled before the bridge replied");
            return false;
        }
        network_pump();
        const NetworkOperationStatus operation = network_control_status();
        if (operation == NETWORK_OPERATION_SUCCEEDED) {
            return true;
        }
        if (operation == NETWORK_OPERATION_FAILED) {
            set_pairing_message("%.140s", network_control_error());
            network_control_consume();
            return false;
        }
        render_frame();
    }
    network_control_cancel();
    return false;
}

/*
 * Exchanges the one-time bootstrap for a revocable device key and stores it.
 * The bootstrap itself is never written to the SD card.
 */
static bool pairing_exchange(const PairingBootstrap *bootstrap)
{
    char body[224];
    const int body_size = snprintf(
        body,
        sizeof(body),
        "{\"code\":\"%s\",\"deviceName\":\"%s\"}",
        bootstrap->code,
        handheld_display_name()
    );
    if (body_size < 0 || (size_t)body_size >= sizeof(body)) {
        set_pairing_message("That pairing code is too long to send");
        return false;
    }

    pairing_phase = UI_PAIRING_EXCHANGING;
    snprintf(
        pairing_bridge_line,
        sizeof(pairing_bridge_line),
        "%s at %s:%u",
        bootstrap->bridge_name,
        bootstrap->host,
        (unsigned int)bootstrap->http_port
    );
    set_pairing_message("Asking %s to pair...", bootstrap->bridge_name);
    set_view_state("Pairing...");

    /* The bootstrap endpoint is a different machine from any current pairing. */
    network_reset_connections();
    if (!network_control_begin_post(
            bootstrap->host,
            bootstrap->http_port,
            "/v1/pair",
            "application/json",
            body,
            (size_t)body_size,
            pairing_message,
            sizeof(pairing_message)
        ) || !pairing_wait_for_control()) {
        return false;
    }

    const unsigned int status = network_control_http_status();
    if (status != 201 && status != 200) {
        char code[48] = "";
        json_read_string(network_control_response(), "message", code, sizeof(code));
        set_pairing_message(
            "%s",
            code[0] != '\0' ? code : "The bridge refused this pairing code"
        );
        network_control_consume();
        return false;
    }

    PairingRecord record;
    memset(&record, 0, sizeof(record));
    const char *response_body = network_control_response();
    const bool parsed = json_read_string(
            response_body,
            "deviceToken",
            record.token,
            sizeof(record.token)
        ) && json_read_string(
            response_body,
            "deviceId",
            record.device_id,
            sizeof(record.device_id)
        );
    unsigned int http_port = bootstrap->http_port;
    unsigned int push_port = bootstrap->push_port;
    json_read_unsigned(response_body, "httpPort", &http_port);
    json_read_unsigned(response_body, "pushPort", &push_port);
    if (!json_read_string(response_body, "host", record.host, sizeof(record.host))) {
        snprintf(record.host, sizeof(record.host), "%s", bootstrap->host);
    }
    if (!json_read_string(
            response_body,
            "bridgeName",
            record.bridge_name,
            sizeof(record.bridge_name)
        )) {
        snprintf(record.bridge_name, sizeof(record.bridge_name), "%s", bootstrap->bridge_name);
    }
    network_control_consume();

    if (!parsed || http_port == 0 || http_port > 65535u
        || push_port == 0 || push_port > 65535u) {
        set_pairing_message("The bridge sent a pairing reply I could not read");
        return false;
    }
    record.http_port = (unsigned short)http_port;
    record.push_port = (unsigned short)push_port;

    time_t now = time(NULL);
    struct tm calendar;
    if (gmtime_r(&now, &calendar) != NULL) {
        strftime(record.paired_at, sizeof(record.paired_at), "%Y-%m-%d", &calendar);
    }
    record.valid = true;

    /*
     * The bridge has issued the credential, so this run is paired either way.
     * A failed write only means it will not survive a relaunch, and saying
     * "not paired" here would contradict what the user can go on to do.
     */
    char save_error[96] = "";
    const bool saved = pairing_save(&record, save_error, sizeof(save_error));
    apply_pairing(&record);
    pairing_phase = UI_PAIRING_SUCCEEDED;
    snprintf(
        pairing_bridge_line,
        sizeof(pairing_bridge_line),
        "%s at %s:%u",
        record.bridge_name,
        record.host,
        (unsigned int)record.http_port
    );
    if (saved) {
        set_pairing_message("Paired with %s", record.bridge_name);
        set_view_state("Paired");
    } else {
        set_pairing_message(
            "Paired with %s, but the key was not saved: %.80s",
            record.bridge_name,
            save_error
        );
        set_view_state("Paired for this session only");
    }
    return true;
}

/* Waits on the pairing screen so the outcome is readable before moving on. */
static void pairing_acknowledge(void)
{
    render_frame();
    while (aptMainLoop()) {
        hidScanInput();
        const u32 keys_down = hidKeysDown();
        update_touch(keys_down, hidKeysHeld(), hidKeysUp());
        const u32 keys = keys_down | touch_virtual_key();
        if ((keys & (KEY_A | KEY_B | KEY_START)) != 0) {
            return;
        }
        network_pump();
        render_frame();
    }
}

/* `cancelled` separates "the user backed out" from "that code was unusable". */
static bool read_pairing_code_by_hand(
    PairingBootstrap *bootstrap,
    bool *cancelled
)
{
    static char typed[128];
    SwkbdState keyboard;
    *cancelled = false;
    swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, (int)sizeof(typed) - 1);
    swkbdSetInitialText(&keyboard, typed);
    swkbdSetHintText(&keyboard, "host port pushport code");
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Pair", true);
    swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

    if (swkbdInputText(&keyboard, typed, sizeof(typed)) != SWKBD_BUTTON_RIGHT) {
        *cancelled = true;
        return false;
    }
    return pairing_parse_manual(
        typed,
        bootstrap,
        pairing_message,
        sizeof(pairing_message)
    );
}

static bool run_manual_pairing(void)
{
    current_screen = UI_SCREEN_PAIRING;
    pairing_preview_ready = false;
    pairing_phase = UI_PAIRING_DECODED;
    set_pairing_message("Type the four values the bridge printed");
    set_view_state("Enter a pairing code");

    PairingBootstrap bootstrap;
    bool cancelled = false;
    if (!read_pairing_code_by_hand(&bootstrap, &cancelled)) {
        if (cancelled) {
            set_view_state("Pairing cancelled");
            return false;
        }
        pairing_phase = UI_PAIRING_FAILED;
        set_view_state("That code could not be read");
        pairing_acknowledge();
        return false;
    }
    if (!pairing_exchange(&bootstrap)) {
        pairing_phase = UI_PAIRING_FAILED;
        set_view_state("Pairing failed");
        pairing_acknowledge();
        return false;
    }
    pairing_acknowledge();
    return true;
}

/*
 * The viewfinder loop. Camera delivery, QR decoding and the network pump all
 * advance once per frame; the decode itself runs on the worker thread, so a
 * frame that takes hundreds of milliseconds to analyse never stalls this loop.
 */
static bool run_qr_pairing(void)
{
    current_screen = UI_SCREEN_PAIRING;
    pairing_phase = UI_PAIRING_AIMING;
    pairing_preview_ready = false;
    set_pairing_message("Point the outer camera at the QR code");
    set_view_state("Scanning for a QR code");
    camera_detail[0] = '\0';
    render_frame();

    u8 *frame = malloc(THREEGENT_PHOTO_BYTES);
    if (frame == NULL) {
        pairing_phase = UI_PAIRING_FAILED;
        set_pairing_message("Not enough memory to open the camera");
        set_view_state("Camera error");
        pairing_acknowledge();
        return false;
    }

    if (!camera_capture_initialize(camera_detail, sizeof(camera_detail))
        || !camera_capture_stream_begin(
            frame,
            THREEGENT_PHOTO_BYTES,
            camera_detail,
            sizeof(camera_detail)
        )
        || !qr_scanner_begin(camera_detail, sizeof(camera_detail))) {
        camera_capture_stream_end();
        qr_scanner_end();
        free(frame);
        pairing_phase = UI_PAIRING_FAILED;
        set_pairing_message("%.140s", camera_detail);
        set_view_state("Camera error");
        pairing_acknowledge();
        return false;
    }

    bool paired = false;
    bool retype = false;
    bool cancelled = false;
    unsigned int reported_recoveries = 0;
    PairingBootstrap bootstrap;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 keys_down = hidKeysDown();
        update_touch(keys_down, hidKeysHeld(), hidKeysUp());
        const u32 keys = keys_down | touch_virtual_key();
        network_pump();

        if ((keys & (KEY_B | KEY_START)) != 0) {
            cancelled = true;
            break;
        }
        if ((keys & KEY_Y) != 0) {
            retype = true;
            break;
        }

        bool frame_ready = false;
        if (!camera_capture_stream_read(
                &frame_ready,
                camera_detail,
                sizeof(camera_detail)
            )) {
            pairing_phase = UI_PAIRING_FAILED;
            set_pairing_message("%.140s", camera_detail);
            break;
        }
        if (frame_ready) {
            /*
             * The camera only writes while a transfer is armed, so the frame is
             * ours until release: upload it and hand a copy to the decoder
             * first, then ask for the next one.
             */
            pairing_preview_ready = ui_photo_preview_set(
                frame,
                THREEGENT_PHOTO_WIDTH,
                THREEGENT_PHOTO_HEIGHT
            );
            qr_scanner_submit(frame, THREEGENT_PHOTO_WIDTH, THREEGENT_PHOTO_HEIGHT);
            if (!camera_capture_stream_release(
                    camera_detail,
                    sizeof(camera_detail)
                )) {
                pairing_phase = UI_PAIRING_FAILED;
                set_pairing_message("%.140s", camera_detail);
                break;
            }
        }

        const unsigned int recoveries = camera_capture_stream_recoveries();
        if (recoveries != reported_recoveries) {
            reported_recoveries = recoveries;
            snprintf(
                camera_detail,
                sizeof(camera_detail),
                "Camera restarted %u time%s after a stall",
                recoveries,
                recoveries == 1 ? "" : "s"
            );
        }

        const char *payload = qr_scanner_take_payload();
        if (payload != NULL) {
            if (pairing_parse_url(
                    payload,
                    &bootstrap,
                    pairing_message,
                    sizeof(pairing_message)
                )) {
                pairing_phase = UI_PAIRING_DECODED;
                break;
            }
            /* A stray QR code in view is not a failure; keep looking. */
            set_view_state("That code is not from 3gent");
        }

        render_frame();
    }

    camera_capture_stream_end();
    qr_scanner_end();
    ui_photo_preview_clear();
    pairing_preview_ready = false;
    free(frame);

    if (retype) {
        return run_manual_pairing();
    }
    /* Backing out is a choice, not a failure; do not report it as one. */
    if (cancelled) {
        set_view_state("Pairing cancelled");
        return false;
    }
    if (pairing_phase == UI_PAIRING_DECODED) {
        paired = pairing_exchange(&bootstrap);
    }
    if (!paired) {
        pairing_phase = UI_PAIRING_FAILED;
        set_view_state("Not paired");
    }
    pairing_acknowledge();
    return paired;
}

/* ------------------------------------------------------------- start screen -- */

/* Returns the chosen action; HOME_EXIT also covers closing the application. */
static HomeAction run_home_screen(void)
{
    current_screen = UI_SCREEN_HOME;
    set_view_state(pairing_record.valid ? "Ready to connect" : "No machine paired");
    snprintf(
        network_detail,
        sizeof(network_detail),
        "%s",
        pairing_record.valid
            ? pairing_storage_path()
            : "Pair with a machine, or use the built-in development host"
    );
    event_detail[0] = '\0';

    while (aptMainLoop()) {
        hidScanInput();
        const u32 keys_down = hidKeysDown();
        update_touch(keys_down, hidKeysHeld(), hidKeysUp());
        u32 keys = keys_down;
        network_pump();

        /* Tapping a row both moves the highlight and chooses it. */
        if (touch_released.kind == UI_HIT_MENU_ROW
            && touch_released.index < HOME_MENU_COUNT) {
            menu_selected = touch_released.index;
            keys |= KEY_A;
        }

        if ((keys & KEY_START) != 0) {
            return HOME_EXIT;
        }
        if ((keys & (KEY_DUP | KEY_CPAD_UP)) != 0) {
            menu_selected = menu_selected == 0
                ? HOME_MENU_COUNT - 1
                : menu_selected - 1;
        }
        if ((keys & (KEY_DDOWN | KEY_CPAD_DOWN)) != 0) {
            menu_selected = (menu_selected + 1) % HOME_MENU_COUNT;
        }
        if ((keys & KEY_A) != 0 && home_menu_enabled[menu_selected]) {
            return (HomeAction)menu_selected;
        }
        render_frame();
    }
    return HOME_EXIT;
}

static void forget_pairing(void)
{
    char error[96] = "";
    if (pairing_forget(error, sizeof(error))) {
        apply_pairing(NULL);
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Device key deleted. Revoke it on the bridge as well."
        );
        set_view_state("Machine forgotten");
    } else {
        snprintf(network_detail, sizeof(network_detail), "%.140s", error);
        set_view_state("Could not forget");
    }
}

/* --------------------------------------------------------- protocol events -- */

static void handle_protocol_event(const char *event_json)
{
    unsigned int protocol_version = 0;
    unsigned int sequence = 0;
    char type[40];
    if (!json_read_unsigned(
            event_json,
            "protocolVersion",
            &protocol_version
        )
        || protocol_version != THREEGENT_PROTOCOL_VERSION
        || !json_read_unsigned(event_json, "sequence", &sequence)) {
        snprintf(event_detail, sizeof(event_detail), "Events: malformed envelope");
        return;
    }
    if (sequence <= event_cursor) {
        return;
    }
    if (!json_read_string(event_json, "type", type, sizeof(type))) {
        event_cursor = sequence;
        snprintf(event_detail, sizeof(event_detail), "Events: missing type at %u", sequence);
        return;
    }

    if (strcmp(type, "connection.ready") == 0) {
        set_view_state("Bridge connected");
    } else if (strcmp(type, "session.updated") == 0) {
        char state[sizeof(agent_state)];
        if (json_read_string(event_json, "state", state, sizeof(state))) {
            snprintf(agent_state, sizeof(agent_state), "%s", state);
            turn_active = strcmp(state, "working") == 0
                || strcmp(state, "waiting_for_user") == 0;
            if (strcmp(state, "idle") == 0) {
                pending_approval_id[0] = '\0';
                pending_approval_summary[0] = '\0';
                set_view_state("Ready");
            } else if (strcmp(state, "working") == 0) {
                set_view_state("Agent working...");
            } else if (strcmp(state, "waiting_for_user") == 0) {
                set_view_state("Approval required");
            }
        }
    } else if (strcmp(type, "turn.started") == 0) {
        turn_active = true;
        diff_known = false;
        set_view_state("Agent working...");
    } else if (strcmp(type, "assistant.text.delta") == 0) {
        char text[512];
        if (json_read_string(event_json, "text", text, sizeof(text))) {
            append_response(text);
            set_view_state("Receiving response...");
        }
    } else if (strcmp(type, "approval.requested") == 0) {
        json_read_string(
            event_json,
            "approvalId",
            pending_approval_id,
            sizeof(pending_approval_id)
        );
        json_read_string(
            event_json,
            "summary",
            pending_approval_summary,
            sizeof(pending_approval_summary)
        );
        approval_arrived_ms = osGetTime();
        append_response("\nApproval required: ");
        append_response(pending_approval_summary);
        append_response("\n");
        turn_active = true;
        set_view_state("Approval required");
    } else if (strcmp(type, "approval.resolved") == 0) {
        char choice[24];
        if (json_read_string(event_json, "choice", choice, sizeof(choice))) {
            append_response("\nApproval response: ");
            append_response(choice);
            append_response("\n");
        }
        pending_approval_id[0] = '\0';
        pending_approval_summary[0] = '\0';
    } else if (strcmp(type, "capture.accepted") == 0) {
        char kind[24];
        if (json_read_string(event_json, "kind", kind, sizeof(kind))
            && strcmp(kind, "audio") == 0) {
            pending_transcript[0] = '\0';
            transcript_ready = false;
            clear_response();
            append_response("Transcribing audio on the bridge...\n");
            set_view_state("Transcribing audio...");
        } else {
            set_view_state("Capture accepted");
        }
    } else if (strcmp(type, "capture.transcript.delta") == 0) {
        char text[320];
        if (json_read_string(event_json, "text", text, sizeof(text))) {
            size_t used = strlen(pending_transcript);
            size_t available = sizeof(pending_transcript) - used - 1;
            strncat(pending_transcript, text, available);
        }
    } else if (strcmp(type, "capture.transcribed") == 0) {
        transcript_ready = pending_transcript[0] != '\0';
        if (transcript_ready) {
            snprintf(prompt, sizeof(prompt), "%s", pending_transcript);
            clear_response();
            append_response(pending_transcript);
            set_view_state("Transcript ready");
        } else {
            set_view_state("Transcript was empty");
        }
    } else if (strcmp(type, "capture.photo.ready") == 0) {
        photo_pending = true;
        set_view_state("Photo ready - add a prompt");
    } else if (strcmp(type, "capture.attached") == 0) {
        photo_pending = false;
        append_response("\n[Photo attached to this turn]\n");
    } else if (strcmp(type, "turn.interrupted") == 0) {
        append_response("\n[Turn interrupted]\n");
        turn_active = false;
        set_view_state("Turn interrupted");
    } else if (strcmp(type, "turn.diff.updated") == 0) {
        unsigned int files = 0;
        unsigned int additions = 0;
        unsigned int deletions = 0;
        if (json_read_unsigned(event_json, "files", &files)
            && json_read_unsigned(event_json, "additions", &additions)
            && json_read_unsigned(event_json, "deletions", &deletions)) {
            diff_known = true;
            diff_files = files;
            diff_additions = additions;
            diff_deletions = deletions;
            char summary[96];
            snprintf(
                summary,
                sizeof(summary),
                "\n[Diff: %u file%s, +%u/-%u]\n",
                files,
                files == 1 ? "" : "s",
                additions,
                deletions
            );
            append_response(summary);
        }
    } else if (strcmp(type, "turn.completed") == 0) {
        char outcome[24];
        turn_active = false;
        if (json_read_string(event_json, "outcome", outcome, sizeof(outcome))
            && strcmp(outcome, "completed") != 0) {
            set_view_state("Turn ended");
        } else {
            set_view_state("Response complete");
        }
    }

    event_cursor = sequence;
    network_push_set_cursor(event_cursor);
}

static bool apply_session_snapshot(const char *snapshot)
{
    unsigned int protocol_version = 0;
    unsigned int latest_sequence = 0;
    char state[sizeof(agent_state)];
    if (!json_read_unsigned(
            snapshot,
            "protocolVersion",
            &protocol_version
        )
        || protocol_version != THREEGENT_PROTOCOL_VERSION
        || !json_read_unsigned(snapshot, "lastSequence", &latest_sequence)
        || !json_read_string(snapshot, "state", state, sizeof(state))) {
        snprintf(event_detail, sizeof(event_detail), "Resync: malformed session snapshot");
        return false;
    }

    event_cursor = latest_sequence;
    snprintf(agent_state, sizeof(agent_state), "%s", state);
    turn_active = strcmp(state, "working") == 0
        || strcmp(state, "waiting_for_user") == 0;
    pending_approval_id[0] = '\0';
    pending_approval_summary[0] = '\0';
    json_read_string(
        snapshot,
        "pendingApprovalId",
        pending_approval_id,
        sizeof(pending_approval_id)
    );
    if (pending_approval_id[0] != '\0') {
        snprintf(
            pending_approval_summary,
            sizeof(pending_approval_summary),
            "Pending approval after event resync"
        );
        approval_arrived_ms = osGetTime();
    }

    clear_response();
    append_response("[Event history changed; session state resynchronized.]\n");
    network_push_set_cursor(event_cursor);
    snprintf(event_detail, sizeof(event_detail), "Events: resynced at %u", event_cursor);
    set_view_state("Session resynchronized");
    return true;
}

static void handle_push_frame(const char *frame)
{
    char type[40];
    if (!json_read_string(frame, "type", type, sizeof(type))) {
        snprintf(event_detail, sizeof(event_detail), "Push: malformed frame");
        set_view_state("Control link error");
        return;
    }

    if (strcmp(type, "connection.ready") == 0) {
        snprintf(event_detail, sizeof(event_detail), "Events: pushed link ready");
        set_view_state("Bridge connected");
    } else if (strcmp(type, "event") == 0) {
        const char *event_key = strstr(frame, "\"event\":");
        const char *event_object = event_key != NULL
            ? strchr(event_key, '{')
            : NULL;
        if (event_object == NULL) {
            snprintf(event_detail, sizeof(event_detail), "Push: missing event");
            set_view_state("Control link error");
        } else {
            handle_protocol_event(event_object);
        }
    } else if (strcmp(type, "command.ack") == 0) {
        char command_id[65];
        bool matched = json_read_string(
                frame,
                "commandId",
                command_id,
                sizeof(command_id)
            ) && network_push_acknowledge(command_id);
        if (!matched) {
            snprintf(
                event_detail,
                sizeof(event_detail),
                "Push: ignored stale acknowledgement"
            );
            return;
        }
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Command accepted in %u ms",
            (unsigned int)(osGetTime() - push_command_started_ms)
        );
        if (push_command_kind == PUSH_COMMAND_TEXT_CAPTURE) {
            set_view_state("Capture accepted");
        } else if (push_command_kind == PUSH_COMMAND_APPROVAL) {
            set_view_state("Approval sent");
        } else if (push_command_kind == PUSH_COMMAND_INTERRUPT) {
            set_view_state("Interrupt sent");
        }
        push_command_kind = PUSH_COMMAND_NONE;
        push_command_started_ms = 0;
    } else if (strcmp(type, "resync.required") == 0) {
        if (!apply_session_snapshot(frame)) {
            snprintf(event_detail, sizeof(event_detail), "Push: malformed resync");
            set_view_state("Session resync failed");
        }
    } else if (strcmp(type, "error") == 0) {
        char message[128];
        if (!json_read_string(frame, "message", message, sizeof(message))) {
            snprintf(message, sizeof(message), "bridge rejected control frame");
        }
        snprintf(network_detail, sizeof(network_detail), "Push: %.120s", message);
        if (push_command_kind == PUSH_COMMAND_TEXT_CAPTURE) {
            set_view_state("Capture error - retry A");
        } else {
            set_view_state("Command failed");
        }
        push_command_kind = PUSH_COMMAND_NONE;
        push_command_started_ms = 0;
    }
}

static void update_push_link(void)
{
    static char previous_state[16];
    const char *state = network_push_state();
    if (strcmp(previous_state, state) != 0) {
        snprintf(previous_state, sizeof(previous_state), "%s", state);
        if (strcmp(state, "retrying") == 0
            || strcmp(state, "connecting") == 0) {
            snprintf(
                event_detail,
                sizeof(event_detail),
                "Push: %s%s%s",
                state,
                network_push_error()[0] != '\0' ? " - " : "",
                network_push_error()
            );
        }
    }

    if (network_push_has_frame()) {
        handle_push_frame(network_push_frame());
        network_push_consume_frame();
    }
}

/* ---------------------------------------------------------------- commands -- */

static SwkbdButton read_prompt(SwkbdResult *keyboard_result)
{
    SwkbdState keyboard;
    swkbdInit(
        &keyboard,
        SWKBD_TYPE_NORMAL,
        2,
        (int)sizeof(prompt) - 1
    );
    swkbdSetInitialText(&keyboard, prompt);
    swkbdSetHintText(&keyboard, "Send a prompt to the selected coding agent");
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Send", true);
    swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&keyboard, SWKBD_PREDICTIVE_INPUT);

    SwkbdButton button = swkbdInputText(&keyboard, prompt, sizeof(prompt));
    *keyboard_result = swkbdGetResult(&keyboard);
    return button;
}

static bool restart_push_link(void)
{
    if (!network_ready) {
        return false;
    }
    if (!network_push_start(
            active_host,
            active_push_port,
            current_session_id,
            event_cursor,
            network_detail,
            sizeof(network_detail)
        )) {
        set_view_state("Push link unavailable");
        return false;
    }
    return true;
}

static void submit_text_capture(const char *text)
{
    transcript_ready = false;
    pending_transcript[0] = '\0';
    clear_response();
    diff_known = false;
    if (network_ready) {
        network_detail[0] = '\0';
    }

    if (text != prompt) {
        snprintf(prompt, sizeof(prompt), "%s", text);
    }
    set_view_state("Submitting capture...");
    render_frame();

    if (!network_ready) {
        set_response("Network initialization failed: %s", network_detail);
        set_view_state("Error - retry A/X");
    } else {
        if (network_push_send_text(
                text,
                network_detail,
                sizeof(network_detail)
            )) {
            push_command_kind = PUSH_COMMAND_TEXT_CAPTURE;
            push_command_started_ms = osGetTime();
            set_view_state(
                network_push_is_ready()
                    ? "Sending capture..."
                    : "Capture queued for reconnect"
            );
        } else {
            set_response("%s", network_detail);
            set_view_state("Capture error - retry A");
        }
    }
}

static void respond_to_approval(const char *choice)
{
    if (pending_approval_id[0] == '\0') {
        return;
    }
    if (!network_push_send_approval(
            pending_approval_id,
            choice,
            network_detail,
            sizeof(network_detail)
        )) {
        set_view_state("Approval error");
        return;
    }
    push_command_kind = PUSH_COMMAND_APPROVAL;
    push_command_started_ms = osGetTime();
    set_view_state("Sending approval...");
}

static void interrupt_active_turn(void)
{
    if (!turn_active) {
        return;
    }
    if (!network_push_send_interrupt(
            network_detail,
            sizeof(network_detail)
        )) {
        set_view_state("Interrupt error");
        return;
    }
    push_command_kind = PUSH_COMMAND_INTERRUPT;
    push_command_started_ms = osGetTime();
    set_view_state("Sending interrupt...");
}

/* ----------------------------------------------------------- voice capture -- */

static void stop_failed_microphone_stream(const char *error_message)
{
    char stop_error[80];
    if (microphone_is_sampling()) {
        microphone_finish_capture(stop_error, sizeof(stop_error));
    }
    network_audio_stream_abort();
    microphone_stream_size = 0;
    recording_session_active = false;
    microphone_stop_requested = false;
    microphone_maximum_reached = false;
    microphone_network_finish_requested = false;
    microphone_link_ready = false;

    if (error_message != NULL && error_message[0] != '\0') {
        set_response("%s", error_message);
    } else {
        set_response("Microphone stream failed.");
    }
    set_view_state("Audio stream error");
}

static bool send_microphone_stream_buffer(bool force, bool *sent)
{
    *sent = false;
    if (microphone_stream_size == 0) {
        return true;
    }
    if (!force && microphone_stream_size < MIC_STREAM_SEND_THRESHOLD) {
        return true;
    }
    if (!network_audio_stream_can_write()) {
        return true;
    }

    if (!network_audio_stream_write(
            microphone_stream_buffer,
            microphone_stream_size,
            network_detail,
            sizeof(network_detail)
        )) {
        return false;
    }

    microphone_stream_size = 0;
    *sent = true;
    return true;
}

static bool drain_microphone_samples(bool force, bool *drained)
{
    *drained = false;
    while (true) {
        if (microphone_stream_size == sizeof(microphone_stream_buffer)) {
            bool sent = false;
            if (!send_microphone_stream_buffer(true, &sent)) {
                return false;
            }
            if (!sent) {
                return true;
            }
        }

        size_t bytes_read = 0;
        if (!microphone_read_capture(
                microphone_stream_buffer + microphone_stream_size,
                sizeof(microphone_stream_buffer) - microphone_stream_size,
                &bytes_read,
                microphone_detail,
                sizeof(microphone_detail)
            )) {
            return false;
        }
        microphone_stream_size += bytes_read;

        bool sent = false;
        if (!send_microphone_stream_buffer(force, &sent)) {
            return false;
        }
        if (bytes_read == 0) {
            *drained = microphone_stream_size == 0
                && network_audio_stream_can_write();
            return true;
        }
        if (!sent && microphone_stream_size >= MIC_STREAM_SEND_THRESHOLD) {
            return true;
        }
    }
}

static void begin_microphone_capture(void)
{
    response_scroll_lines = 0;
    if (network_ready) {
        network_detail[0] = '\0';
    }
    microphone_detail[0] = '\0';

    if (!network_ready) {
        set_response("Network initialization failed: %s", network_detail);
        set_view_state("Audio stream error");
        return;
    }

    if (!microphone_begin_capture(
            microphone_detail,
            sizeof(microphone_detail)
        )) {
        set_response("%s", microphone_detail);
        set_view_state("Microphone error");
        return;
    }

    microphone_stream_size = 0;
    microphone_stop_requested = false;
    microphone_maximum_reached = false;
    microphone_network_finish_requested = false;
    microphone_link_ready = false;
    microphone_link_latency_ms = 0;
    microphone_finish_started_ms = 0;
    clear_response();
    diff_known = false;
    recording_session_active = true;
    set_view_state("Connecting audio...");
    render_frame();

    microphone_link_started_ms = osGetTime();
    if (!network_audio_stream_begin(
            active_host,
            active_port,
            audio_capture_path,
            network_detail,
            sizeof(network_detail)
        )) {
        char stream_error[sizeof(network_detail)];
        snprintf(stream_error, sizeof(stream_error), "%s", network_detail);
        microphone_finish_capture(
            microphone_detail,
            sizeof(microphone_detail)
        );
        recording_session_active = false;
        microphone_stream_size = 0;
        set_response("%s", stream_error);
        set_view_state("Audio stream error");
        return;
    }
}

static void request_microphone_finish(bool maximum_reached)
{
    if (microphone_stop_requested) {
        return;
    }
    if (!microphone_finish_capture(
            microphone_detail,
            sizeof(microphone_detail)
        )) {
        stop_failed_microphone_stream(microphone_detail);
        return;
    }
    microphone_stop_requested = true;
    microphone_maximum_reached = maximum_reached;
    microphone_finish_started_ms = osGetTime();
    set_view_state("Finalizing audio...");
}

static void update_microphone_capture(void)
{
    NetworkOperationStatus status = network_audio_stream_status();
    if (status == NETWORK_OPERATION_FAILED) {
        snprintf(
            network_detail,
            sizeof(network_detail),
            "%s",
            network_audio_stream_error()
        );
        stop_failed_microphone_stream(network_detail);
        return;
    }
    if (status == NETWORK_OPERATION_SUCCEEDED) {
        size_t total_pcm_size = microphone_total_pcm_size();
        network_audio_stream_consume();
        recording_session_active = false;
        microphone_stream_size = 0;
        microphone_stop_requested = false;
        microphone_network_finish_requested = false;
        microphone_link_ready = false;
        snprintf(
            microphone_detail,
            sizeof(microphone_detail),
            "Held %u ms; captured %u ms (%u bytes, %u offset changes)",
            microphone_wall_duration_ms(),
            microphone_duration_ms(),
            (unsigned int)total_pcm_size,
            microphone_offset_change_count()
        );
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Audio accepted %u ms after release (link %u ms)",
            (unsigned int)(osGetTime() - microphone_finish_started_ms),
            microphone_link_latency_ms
        );
        if (!transcript_ready) {
            set_view_state(
                microphone_maximum_reached
                    ? "5-minute audio transcribing"
                    : "Audio transcribing"
            );
        }
        microphone_maximum_reached = false;
        return;
    }
    if (status != NETWORK_OPERATION_IN_PROGRESS) {
        stop_failed_microphone_stream("Audio stream stopped unexpectedly.");
        return;
    }

    bool stream_ready = network_audio_stream_is_ready();
    bool drained = false;
    if (!drain_microphone_samples(microphone_stop_requested, &drained)) {
        const char *detail = microphone_detail[0] != '\0'
            ? microphone_detail
            : network_detail;
        stop_failed_microphone_stream(detail);
        return;
    }
    if (!microphone_stop_requested && microphone_capture_is_full()) {
        request_microphone_finish(true);
        return;
    }

    if (!stream_ready) {
        set_view_state(
            microphone_stop_requested
                ? "Finalizing audio..."
                : "Connecting audio..."
        );
        return;
    }

    if (!microphone_link_ready) {
        microphone_link_ready = true;
        microphone_link_latency_ms = (unsigned int)(
            osGetTime() - microphone_link_started_ms
        );
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Audio link ready in %u ms",
            microphone_link_latency_ms
        );
        set_view_state(
            microphone_stop_requested
                ? "Finalizing audio..."
                : "Recording..."
        );
    }

    if (microphone_stop_requested && drained
        && !microphone_network_finish_requested) {
        if (microphone_total_pcm_size() == 0) {
            stop_failed_microphone_stream("No microphone samples were captured.");
            return;
        }
        if (!network_audio_stream_finish(
                network_detail,
                sizeof(network_detail)
            )) {
            stop_failed_microphone_stream(network_detail);
            return;
        }
        microphone_network_finish_requested = true;
    }
}

/* ---------------------------------------------------------- camera capture -- */

static void capture_and_upload_photo(void)
{
    camera_detail[0] = '\0';
    u8 *photo = malloc(THREEGENT_PHOTO_BYTES);
    if (photo == NULL) {
        snprintf(camera_detail, sizeof(camera_detail), "Could not allocate photo buffer");
        set_view_state("Camera error");
        return;
    }

    set_view_state("Opening the camera...");
    render_frame();
    if (!camera_capture_initialize(camera_detail, sizeof(camera_detail))
        || !camera_capture_photo(
            photo,
            THREEGENT_PHOTO_BYTES,
            camera_detail,
            sizeof(camera_detail)
        )) {
        set_view_state("Camera capture error");
        free(photo);
        return;
    }

    const UiScreen previous_screen = current_screen;
    current_screen = UI_SCREEN_PHOTO;
    photo_progress_percent = UI_PHOTO_PROGRESS_NONE;
    if (!ui_photo_preview_set(
            photo,
            THREEGENT_PHOTO_WIDTH,
            THREEGENT_PHOTO_HEIGHT
        )) {
        snprintf(camera_detail, sizeof(camera_detail), "Preview upload failed");
    }
    photo_caption = "Attach this shot to your next prompt";
    set_view_state("Review the photo");

    bool accepted = false;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 keys_down = hidKeysDown();
        update_touch(keys_down, hidKeysHeld(), hidKeysUp());
        const u32 keys = keys_down | touch_virtual_key();
        network_pump();
        update_push_link();
        render_frame();
        if ((keys & KEY_A) != 0) {
            accepted = true;
            break;
        }
        if ((keys & (KEY_B | KEY_START)) != 0) {
            break;
        }
    }
    if (!accepted) {
        free(photo);
        ui_photo_preview_clear();
        current_screen = previous_screen;
        set_view_state("Photo discarded");
        return;
    }

    if (!network_photo_upload_begin(
            active_host,
            active_port,
            photo_capture_path,
            camera_detail,
            sizeof(camera_detail)
        )) {
        free(photo);
        ui_photo_preview_clear();
        current_screen = previous_screen;
        set_view_state("Photo upload error");
        return;
    }
    size_t offset = 0;
    bool finish_requested = false;
    bool complete = false;
    photo_progress_percent = 0;
    photo_caption = "Uploading to the bridge...";
    set_view_state("Uploading photo...");
    while (aptMainLoop()) {
        hidScanInput();
        if ((hidKeysDown() & KEY_START) != 0) {
            network_photo_upload_abort();
            break;
        }
        network_pump();
        update_push_link();
        NetworkOperationStatus status = network_photo_upload_status();
        if (status == NETWORK_OPERATION_FAILED) {
            snprintf(camera_detail, sizeof(camera_detail), "%s", network_photo_upload_error());
            network_photo_upload_consume();
            break;
        }
        if (status == NETWORK_OPERATION_SUCCEEDED) {
            network_photo_upload_consume();
            complete = true;
            break;
        }
        if (offset < THREEGENT_PHOTO_BYTES && network_photo_upload_can_write()) {
            size_t remaining = THREEGENT_PHOTO_BYTES - offset;
            size_t chunk = remaining > MIC_STREAM_BUFFER_CAPACITY
                ? MIC_STREAM_BUFFER_CAPACITY
                : remaining;
            if (!network_photo_upload_write(
                    photo + offset,
                    chunk,
                    camera_detail,
                    sizeof(camera_detail)
                )) {
                network_photo_upload_abort();
                break;
            }
            offset += chunk;
        } else if (offset == THREEGENT_PHOTO_BYTES && !finish_requested) {
            if (!network_photo_upload_finish(camera_detail, sizeof(camera_detail))) {
                network_photo_upload_abort();
                break;
            }
            finish_requested = true;
        }
        photo_progress_percent =
            (unsigned int)(offset * 100u / THREEGENT_PHOTO_BYTES);
        render_frame();
    }
    free(photo);
    ui_photo_preview_clear();
    photo_progress_percent = UI_PHOTO_PROGRESS_NONE;
    current_screen = previous_screen;
    if (complete) {
        photo_pending = true;
        snprintf(camera_detail, sizeof(camera_detail), "Uploaded 400x240 RGB565 photo");
        set_response("Photo ready. Type or speak the prompt that should include it.");
        set_view_state("Photo attached");
    } else {
        set_view_state("Photo upload failed");
    }
}

/* ---------------------------------------------------------------- scrolling -- */

static void change_scroll(bool scroll_up)
{
    size_t max_scroll = get_max_scroll();
    if (scroll_up && response_scroll_lines < max_scroll) {
        response_scroll_lines++;
    } else if (!scroll_up && response_scroll_lines > 0) {
        response_scroll_lines--;
    }
}

/* The on-screen scroll controls move a page, because a tap should be worth it. */
static void scroll_by_page(bool scroll_up)
{
    UiModel model;
    build_model(&model);
    const size_t max_scroll = ui_max_scroll(&model);
    const size_t page = ui_page_lines(&model);
    if (scroll_up) {
        response_scroll_lines = response_scroll_lines + page > max_scroll
            ? max_scroll
            : response_scroll_lines + page;
    } else {
        response_scroll_lines = response_scroll_lines > page
            ? response_scroll_lines - page
            : 0;
    }
}

static void handle_scroll_input(u32 keys_down, u32 keys_held)
{
    const u32 scroll_up_keys = KEY_DUP | KEY_CPAD_UP;
    const u32 scroll_down_keys = KEY_DDOWN | KEY_CPAD_DOWN;
    const u32 active_keys = keys_down | keys_held;
    const bool up_held = (active_keys & scroll_up_keys) != 0;
    const bool down_held = (active_keys & scroll_down_keys) != 0;
    int direction = 0;

    if (up_held && !down_held) {
        direction = 1;
    } else if (down_held && !up_held) {
        direction = -1;
    }

    if (direction == 0) {
        scroll_repeat_direction = 0;
        scroll_repeat_frames = 0;
        return;
    }

    const bool direction_pressed = direction > 0
        ? (keys_down & scroll_up_keys) != 0
        : (keys_down & scroll_down_keys) != 0;

    if (direction_pressed || direction != scroll_repeat_direction) {
        change_scroll(direction > 0);
        scroll_repeat_direction = direction;
        scroll_repeat_frames = 0;
        return;
    }

    scroll_repeat_frames++;
    if (scroll_repeat_frames >= SCROLL_REPEAT_DELAY_FRAMES
        && (scroll_repeat_frames - SCROLL_REPEAT_DELAY_FRAMES)
            % SCROLL_REPEAT_INTERVAL_FRAMES == 0) {
        change_scroll(direction > 0);
    }
}

/* ------------------------------------------------------------ connected run -- */

/*
 * How the agent loop ended. "Back" is one step out to the task manager; only
 * START leaves the machine entirely.
 */
typedef enum {
    AGENT_EXIT_TASKS = 0,
    AGENT_EXIT_HOME,
} AgentExit;

static AgentExit run_agent_loop(void);

/*
 * Connects to the active machine and runs the agent loop until the user leaves.
 * Everything before this point is endpoint selection; nothing after it knows or
 * cares whether the endpoint came from a pairing or from the build constants.
 */
static void run_connected_session(void)
{
    if (!network_ready) {
        current_screen = UI_SCREEN_HOME;
        set_view_state("No network");
        return;
    }

    current_screen = UI_SCREEN_BOOT;
    set_view_state("Warming low-latency links...");
    render_frame();

    const u64 warmup_started_ms = osGetTime();
    if (network_prepare_connections(
            active_host,
            active_port,
            network_detail,
            sizeof(network_detail)
        )) {
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Audio link ready in %u ms",
            (unsigned int)(osGetTime() - warmup_started_ms)
        );
        set_view_state("Ready");
    } else {
        set_view_state("Server offline - retry action");
    }

    /*
     * Task manager and agent loop alternate. B in the agent loop returns here
     * rather than to the start screen, so "back" is one consistent step out.
     */
    while (aptMainLoop()) {
        if (run_task_manager() == TASKS_BACK) {
            break;
        }
        if (run_agent_loop() == AGENT_EXIT_HOME) {
            break;
        }
    }

    network_push_stop();
    network_reset_connections();
    turn_active = false;
    transcript_ready = false;
    pending_approval_id[0] = '\0';
    pending_approval_summary[0] = '\0';
    photo_pending = false;
    clear_response();
    prompt[0] = '\0';
    task_count = 0;
    task_active_valid = false;
    current_session_id[0] = '\0';
    current_session_label[0] = '\0';
    snprintf(agent_state, sizeof(agent_state), "connecting");
    current_screen = UI_SCREEN_HOME;
}

/* Moves `step` tasks along the rail, wrapping, and opens what it lands on. */
static void step_to_adjacent_task(int step)
{
    if (task_count < 2 || !task_active_valid) {
        return;
    }
    const size_t next = step > 0
        ? (task_active + 1) % task_count
        : (task_active == 0 ? task_count - 1 : task_active - 1);
    open_task(next);
}

static AgentExit run_agent_loop(void)
{
    current_screen = UI_SCREEN_MAIN;
    task_refresh_checked_ms = osGetTime();

    while (aptMainLoop()) {
        hidScanInput();
        u32 keys_down = hidKeysDown();
        u32 keys_held = hidKeysHeld();
        u32 keys_up = hidKeysUp();

        network_pump();
        update_push_link();
        update_task_refresh();
        update_touch(keys_down, keys_held, keys_up);

        /*
         * Touch is resolved to the action it was drawn as, then folded into the
         * key that already implements it. Rail and scroll targets have no key
         * equivalent, so they act here.
         */
        keys_down |= touch_virtual_key();
        if (touch_released.kind == UI_HIT_TASK) {
            open_task(touch_released.index);
            render_frame();
            continue;
        }
        if (touch_released.kind == UI_HIT_TASK_LIST) {
            return AGENT_EXIT_TASKS;
        }
        if (touch_released.kind == UI_HIT_SCROLL_BACK) {
            scroll_by_page(true);
        } else if (touch_released.kind == UI_HIT_SCROLL_FORWARD) {
            scroll_by_page(false);
        } else if (touch_released.kind == UI_HIT_SCROLL_LATEST) {
            response_scroll_lines = 0;
        }
        /* Holding the on-screen panel is the stylus form of holding R. */
        if (touch_talk_held(keys_held)) {
            keys_held |= KEY_R;
            if ((keys_down & KEY_TOUCH) != 0) {
                keys_down |= KEY_R;
            }
        }
        if (recording_session_active
            && touch_armed.kind == UI_HIT_TALK
            && (keys_up & KEY_TOUCH) != 0) {
            keys_up |= KEY_R;
        }

        if ((keys_down & KEY_START) != 0) {
            return AGENT_EXIT_HOME;
        }

        /* Left and right walk the rail without leaving the conversation. */
        if (!recording_session_active && !transcript_ready) {
            if ((keys_down & (KEY_DLEFT | KEY_CPAD_LEFT)) != 0) {
                step_to_adjacent_task(-1);
            } else if ((keys_down & (KEY_DRIGHT | KEY_CPAD_RIGHT)) != 0) {
                step_to_adjacent_task(1);
            }
        }

        if ((keys_down & KEY_R) != 0 && !recording_session_active) {
            if (transcript_ready) {
                snprintf(network_detail, sizeof(network_detail), "Send or cancel the reviewed transcript first");
                set_view_state("Transcript awaiting decision");
            } else if (turn_active) {
                snprintf(
                    network_detail,
                    sizeof(network_detail),
                    "Wait for or interrupt the active turn"
                );
                set_view_state("Agent is busy");
            } else {
                begin_microphone_capture();
            }
        }

        if ((keys_down & KEY_L) != 0 && !recording_session_active) {
            if (turn_active || transcript_ready || push_command_kind != PUSH_COMMAND_NONE) {
                snprintf(network_detail, sizeof(network_detail), "Wait for the current action before taking a photo");
                set_view_state("Camera is busy");
            } else {
                capture_and_upload_photo();
            }
        }

        if (recording_session_active) {
            if ((keys_up & KEY_R) != 0) {
                request_microphone_finish(false);
            }
            update_microphone_capture();
            render_frame();
            continue;
        }

        if ((keys_down & KEY_A) != 0) {
            if (pending_approval_id[0] != '\0') {
                /*
                 * Approve is on A so the button grammar holds, but a press
                 * that was already travelling toward something else must not
                 * answer a request that appeared a frame ago.
                 */
                if (osGetTime() - approval_arrived_ms >= APPROVAL_ARMING_MS) {
                    respond_to_approval("approve_once");
                } else {
                    set_view_state("Read it first, then approve");
                }
            } else if (transcript_ready) {
                snprintf(prompt, sizeof(prompt), "%s", pending_transcript);
                submit_text_capture(prompt);
            } else if (turn_active || push_command_kind != PUSH_COMMAND_NONE) {
                snprintf(
                    network_detail,
                    sizeof(network_detail),
                    "%s",
                    turn_active
                        ? "Wait for or interrupt the active turn"
                        : "Wait for the queued command acknowledgement"
                );
                set_view_state("Agent is busy");
            } else {
                /* swkbd is modal, so reconnect afterward instead of timing out. */
                network_push_stop();
                SwkbdResult keyboard_result = SWKBD_NONE;
                SwkbdButton button = read_prompt(&keyboard_result);
                restart_push_link();

                if (button == SWKBD_BUTTON_RIGHT) {
                    submit_text_capture(prompt);
                } else {
                    snprintf(
                        network_detail,
                        sizeof(network_detail),
                        "Keyboard cancelled (result %d)",
                        (int)keyboard_result
                    );
                    set_view_state("Input cancelled");
                }
            }
        }

        if ((keys_down & KEY_Y) != 0 && transcript_ready) {
            network_push_stop();
            snprintf(prompt, sizeof(prompt), "%s", pending_transcript);
            SwkbdResult keyboard_result = SWKBD_NONE;
            SwkbdButton button = read_prompt(&keyboard_result);
            restart_push_link();
            if (button == SWKBD_BUTTON_RIGHT) {
                snprintf(pending_transcript, sizeof(pending_transcript), "%s", prompt);
                clear_response();
                append_response(pending_transcript);
                set_view_state("Transcript edited");
            } else {
                set_view_state("Transcript edit cancelled");
            }
        }

        /* X is the secondary: stop what is running, or start something new. */
        if ((keys_down & KEY_X) != 0 && pending_approval_id[0] == '\0') {
            if (turn_active) {
                interrupt_active_turn();
            } else if (!transcript_ready) {
                start_and_open_new_task();
            }
        }

        /* B is always "back", except where there is a decision to back out of. */
        if ((keys_down & KEY_B) != 0) {
            if (transcript_ready) {
                transcript_ready = false;
                pending_transcript[0] = '\0';
                prompt[0] = '\0';
                clear_response();
                append_response("[Transcript cancelled]\n");
                set_view_state("Ready");
            } else if (pending_approval_id[0] != '\0') {
                respond_to_approval("decline");
            } else {
                return AGENT_EXIT_TASKS;
            }
        }

        handle_scroll_input(keys_down, keys_held);

        render_frame();
    }

    return AGENT_EXIT_HOME;
}

/* --------------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char startup_error[96];
    if (!ui_initialize(startup_error, sizeof(startup_error))) {
        /*
         * Without the renderer there is no way to report anything, so fall back
         * to the text console purely to show why the GPU was unavailable.
         */
        gfxInitDefault();
        consoleInit(GFX_TOP, NULL);
        printf("3gent %s could not start.\n\n%s\n\nPress START to exit.\n",
            THREEGENT_APP_VERSION, startup_error);
        while (aptMainLoop()) {
            hidScanInput();
            if ((hidKeysDown() & KEY_START) != 0) {
                break;
            }
            gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }

    network_ready = network_start(network_detail, sizeof(network_detail));
    /*
     * Synchronous requests draw the screen behind them while they wait, so a
     * spinner keeps turning through a connection warm-up instead of freezing on
     * the frame that was up when the call started.
     */
    network_set_wait_callback(network_wait_redraw, NULL);
    microphone_initialize(microphone_detail, sizeof(microphone_detail));

    PairingRecord saved;
    apply_pairing(pairing_load(&saved) ? &saved : NULL);

    while (aptMainLoop()) {
        const HomeAction action = run_home_screen();
        if (action == HOME_EXIT) {
            break;
        }
        if (action == HOME_PAIR_QR) {
            run_qr_pairing();
        } else if (action == HOME_PAIR_MANUAL) {
            run_manual_pairing();
        } else if (action == HOME_FORGET) {
            forget_pairing();
        } else {
            run_connected_session();
        }
    }

    camera_capture_shutdown();
    qr_scanner_end();
    microphone_shutdown();
    network_stop();
    ui_shutdown();
    return 0;
}
