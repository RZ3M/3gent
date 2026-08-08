#include "app_config.h"
#include "microphone.h"
#include "network.h"

#include <3ds.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESPONSE_WRAP_COLUMNS 48
#define RESPONSE_MAX_LINES 64
#define RESPONSE_VISIBLE_LINES 22
#define SCROLL_REPEAT_DELAY_FRAMES 18
#define SCROLL_REPEAT_INTERVAL_FRAMES 4
#define MIC_STREAM_BUFFER_CAPACITY 8192
#define MIC_STREAM_SEND_THRESHOLD 1024
#define EVENT_POLL_WORKING_FRAMES 6
#define EVENT_POLL_APPROVAL_FRAMES 15
#define EVENT_POLL_IDLE_FRAMES 60
#define EVENT_POLL_RETRY_INITIAL_FRAMES 60
#define EVENT_POLL_RETRY_MAX_FRAMES 600
#define EVENT_POLL_BATCH_LIMIT 3U
#define STAGE1_SESSION_ID "ses_fake_local"
#define STAGE1_TEXT_CAPTURE_PATH \
    "/v1/sessions/" STAGE1_SESSION_ID "/captures/text"
#define STAGE1_AUDIO_CAPTURE_PATH \
    "/v1/sessions/" STAGE1_SESSION_ID "/captures/audio"
#define STAGE1_SESSION_PATH "/v1/sessions/" STAGE1_SESSION_ID
#define STAGE1_INTERRUPT_PATH \
    "/v1/sessions/" STAGE1_SESSION_ID "/turns/current/interrupt"

typedef enum {
    CONTROL_REQUEST_NONE = 0,
    CONTROL_REQUEST_EVENT_POLL,
    CONTROL_REQUEST_SESSION_RESYNC,
    CONTROL_REQUEST_TEXT_CAPTURE,
    CONTROL_REQUEST_APPROVAL,
    CONTROL_REQUEST_INTERRUPT,
} ControlRequestKind;

static PrintConsole top_console;
static PrintConsole bottom_console;

static char prompt[THREEGENT_PROMPT_CAPACITY];
static char response[THREEGENT_RESPONSE_CAPACITY];
static char network_detail[160];
static char microphone_detail[160];
static char agent_state[24] = "connecting";
static char pending_approval_id[65];
static char pending_approval_summary[96];
static char view_state[32] = "Ready";
static char event_detail[96];
static char event_batch[THREEGENT_RESPONSE_CAPACITY];
static char wrapped_lines[RESPONSE_MAX_LINES][RESPONSE_WRAP_COLUMNS + 1];
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
static u32 event_poll_frames;
static u32 event_poll_retry_frames = EVENT_POLL_RETRY_INITIAL_FRAMES;
static u32 event_poll_failure_wait_frames = EVENT_POLL_RETRY_INITIAL_FRAMES;
static unsigned int event_cursor;
static bool turn_active;
static bool event_poll_failed;
static ControlRequestKind control_request_kind;
static u64 control_request_started_ms;
static char control_accepted_state[32];

static void set_view_state(const char *state)
{
    snprintf(view_state, sizeof(view_state), "%s", state);
}

static size_t wrap_response(void)
{
    memset(wrapped_lines, 0, sizeof(wrapped_lines));
    if (response[0] == '\0') {
        return 0;
    }

    size_t line = 0;
    size_t column = 0;
    size_t line_count = 1;

    for (const char *cursor = response; *cursor != '\0'; cursor++) {
        if (*cursor == '\r') {
            continue;
        }

        if (*cursor == '\n') {
            if (line + 1 >= RESPONSE_MAX_LINES) {
                break;
            }
            line++;
            column = 0;
            line_count = line + 1;
            continue;
        }

        if (column >= RESPONSE_WRAP_COLUMNS) {
            if (line + 1 >= RESPONSE_MAX_LINES) {
                break;
            }
            line++;
            column = 0;
            line_count = line + 1;
        }

        wrapped_lines[line][column] = *cursor;
        column++;
        wrapped_lines[line][column] = '\0';
    }

    return line_count;
}

static size_t get_max_scroll(void)
{
    size_t line_count = wrap_response();
    if (line_count <= RESPONSE_VISIBLE_LINES) {
        return 0;
    }
    return line_count - RESPONSE_VISIBLE_LINES;
}

static void draw_top(void)
{
    consoleSelect(&top_console);
    consoleClear();

    printf("3gent | %s\n", THREEGENT_APP_VERSION);
    printf("Stage 1 | Local fake agent\n");
    printf("Agent: %s | event %u\n", agent_state, event_cursor);
    printf("State: %s\n", view_state);
    if (prompt[0] != '\0') {
        printf(
            "Prompt: %.36s%s\n",
            prompt,
            strlen(prompt) > 36 ? "..." : ""
        );
    } else {
        printf("Prompt: none\n");
    }
    printf("------------------------------------------------\n");

    size_t line_count = wrap_response();
    if (line_count == 0) {
        printf("No response yet.\n");
        return;
    }

    size_t latest_start = 0;
    if (line_count > RESPONSE_VISIBLE_LINES) {
        latest_start = line_count - RESPONSE_VISIBLE_LINES;
    }
    if (response_scroll_lines > latest_start) {
        response_scroll_lines = latest_start;
    }

    size_t first_line = latest_start - response_scroll_lines;
    size_t last_line = first_line + RESPONSE_VISIBLE_LINES;
    if (last_line > line_count) {
        last_line = line_count;
    }

    for (size_t line = first_line; line < last_line; line++) {
        printf("%s\n", wrapped_lines[line]);
    }
}

static void draw_bottom(void)
{
    consoleSelect(&bottom_console);
    consoleClear();

    printf("A: Type + send to fake agent\n");
    if (pending_approval_id[0] != '\0') {
        printf("X: Approve once | B: Decline\n");
    } else if (turn_active) {
        printf("B: Interrupt active turn\n");
    } else {
        printf("X: Fake approval demo\n");
    }
    printf("R: Hold to stream mic (5 min max)\n");
    printf("UP/DOWN: Scroll response\n");
    printf("START: Exit\n\n");
    printf("Server: %s:%u\n", THREEGENT_SERVER_HOST, (unsigned int)THREEGENT_SERVER_PORT);
    printf(
        "Network: %s | warm links: %u/2\n",
        network_ready ? "ready" : "unavailable",
        network_warm_connection_count()
    );
    printf(
        "Mic: %s (PCM16 mono, 16364 Hz)\n",
        microphone_is_ready() ? "ready" : "unavailable"
    );

    if (recording_session_active) {
        unsigned int wall_duration_ms = microphone_wall_duration_ms();
        unsigned int pcm_duration_ms = microphone_duration_ms();
        unsigned int level = microphone_level_percent();
        unsigned int level_marks = level / 10;
        if (level_marks > 10) {
            level_marks = 10;
        }

        printf(
            "Held: %u:%02u.%02u / %u:%02u\n",
            wall_duration_ms / 60000,
            (wall_duration_ms / 1000) % 60,
            (wall_duration_ms % 1000) / 10,
            THREEGENT_MIC_MAX_SECONDS / 60,
            THREEGENT_MIC_MAX_SECONDS % 60
        );
        printf(
            "PCM: %u ms | pos %lu | changes %u\n",
            pcm_duration_ms,
            (unsigned long)microphone_last_write_offset(),
            microphone_offset_change_count()
        );
        printf(
            "MICU: %s | no new data: %u ms\n",
            microphone_service_is_sampling() ? "sampling" : "stopped",
            microphone_stall_ms()
        );
        printf("Level: [");
        for (unsigned int mark = 0; mark < 10; mark++) {
            printf("%c", mark < level_marks ? '#' : '.');
        }
        printf("] %u%%\n", level);
    }

    if (response_scroll_lines == 0) {
        printf("Scroll: latest\n");
    } else {
        printf("Scroll: %u lines back\n", (unsigned int)response_scroll_lines);
    }

    if (network_detail[0] != '\0') {
        printf("%s\n", network_detail);
    }
    if (microphone_detail[0] != '\0') {
        printf("%s\n", microphone_detail);
    }
    if (event_detail[0] != '\0') {
        printf("%s\n", event_detail);
    }
}

static void redraw(void)
{
    draw_top();
    draw_bottom();
}

static void present_frame(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void append_response(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    size_t used = strlen(response);
    if (used + 1 >= sizeof(response)) {
        return;
    }
    snprintf(response + used, sizeof(response) - used, "%s", text);
    response_scroll_lines = 0;
}

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
        append_response("\nApproval required: ");
        append_response(pending_approval_summary);
        append_response("\nX: approve once | B: decline\n");
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
        set_view_state("Capture accepted");
    } else if (strcmp(type, "turn.interrupted") == 0) {
        append_response("\n[Turn interrupted]\n");
        turn_active = false;
        set_view_state("Turn interrupted");
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
}

static bool handle_event_batch(char *batch)
{
    bool handled = false;
    char *line = batch;
    while (*line != '\0') {
        char *line_end = strchr(line, '\n');
        if (line_end != NULL) {
            *line_end = '\0';
        }
        if (line[0] != '\0') {
            handle_protocol_event(line);
            handled = true;
        }
        if (line_end == NULL) {
            break;
        }
        line = line_end + 1;
    }
    return handled;
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
    }

    response[0] = '\0';
    append_response("[Event history changed; session state resynchronized.]\n");
    if (pending_approval_id[0] != '\0') {
        append_response("Approval is still pending. X: approve once | B: decline\n");
    }
    event_poll_failed = false;
    event_poll_retry_frames = EVENT_POLL_RETRY_INITIAL_FRAMES;
    event_poll_failure_wait_frames = EVENT_POLL_RETRY_INITIAL_FRAMES;
    snprintf(event_detail, sizeof(event_detail), "Events: resynced at %u", event_cursor);
    set_view_state("Session resynchronized");
    return true;
}

static u32 desired_event_poll_frames(void)
{
    if (event_poll_failed) {
        return event_poll_failure_wait_frames;
    }
    if (pending_approval_id[0] != '\0'
        || strcmp(agent_state, "waiting_for_user") == 0) {
        return EVENT_POLL_APPROVAL_FRAMES;
    }
    if (turn_active || strcmp(agent_state, "working") == 0) {
        return EVENT_POLL_WORKING_FRAMES;
    }
    return EVENT_POLL_IDLE_FRAMES;
}

static void force_event_poll(void)
{
    event_poll_frames = desired_event_poll_frames();
}

static void mark_event_poll_failure(const char *prefix, const char *detail)
{
    snprintf(
        event_detail,
        sizeof(event_detail),
        "%s: %.82s",
        prefix,
        detail != NULL ? detail : "unknown network error"
    );
    event_poll_failed = true;
    event_poll_frames = 0;
    event_poll_failure_wait_frames = event_poll_retry_frames;
    if (event_poll_retry_frames < EVENT_POLL_RETRY_MAX_FRAMES) {
        event_poll_retry_frames *= 2;
        if (event_poll_retry_frames > EVENT_POLL_RETRY_MAX_FRAMES) {
            event_poll_retry_frames = EVENT_POLL_RETRY_MAX_FRAMES;
        }
    }
    set_view_state("Event connection error");
    redraw();
}

static void clear_control_request(void)
{
    network_control_consume();
    control_request_kind = CONTROL_REQUEST_NONE;
    control_request_started_ms = 0;
    control_accepted_state[0] = '\0';
}

static bool start_control_get(ControlRequestKind kind, const char *path)
{
    if (control_request_kind != CONTROL_REQUEST_NONE) {
        return false;
    }
    if (!network_control_begin_get(
            THREEGENT_SERVER_HOST,
            THREEGENT_SERVER_PORT,
            path,
            network_detail,
            sizeof(network_detail)
        )) {
        return false;
    }
    control_request_kind = kind;
    control_request_started_ms = osGetTime();
    return true;
}

static bool start_control_post(
    ControlRequestKind kind,
    const char *path,
    const char *content_type,
    const void *body,
    size_t body_size,
    const char *accepted_state
)
{
    if (control_request_kind == CONTROL_REQUEST_EVENT_POLL
        || control_request_kind == CONTROL_REQUEST_SESSION_RESYNC) {
        network_control_cancel();
        control_request_kind = CONTROL_REQUEST_NONE;
    }
    if (control_request_kind != CONTROL_REQUEST_NONE) {
        snprintf(network_detail, sizeof(network_detail), "Another command is still sending");
        set_view_state("Command busy");
        redraw();
        return false;
    }
    if (!network_control_begin_post(
            THREEGENT_SERVER_HOST,
            THREEGENT_SERVER_PORT,
            path,
            content_type,
            body,
            body_size,
            network_detail,
            sizeof(network_detail)
        )) {
        snprintf(response, sizeof(response), "%s", network_detail);
        set_view_state("Command failed");
        redraw();
        return false;
    }
    control_request_kind = kind;
    control_request_started_ms = osGetTime();
    snprintf(
        control_accepted_state,
        sizeof(control_accepted_state),
        "%s",
        accepted_state
    );
    return true;
}

static void start_session_resync(void)
{
    if (!start_control_get(
            CONTROL_REQUEST_SESSION_RESYNC,
            STAGE1_SESSION_PATH
        )) {
        mark_event_poll_failure("Resync", network_detail);
    }
}

static void finish_control_request(void)
{
    NetworkOperationStatus status = network_control_status();
    if (control_request_kind == CONTROL_REQUEST_NONE
        || status == NETWORK_OPERATION_IDLE
        || status == NETWORK_OPERATION_IN_PROGRESS) {
        return;
    }

    ControlRequestKind completed_kind = control_request_kind;
    bool succeeded = status == NETWORK_OPERATION_SUCCEEDED;
    unsigned int http_status = network_control_http_status();
    const char *body = network_control_response();
    const char *request_error = network_control_error();
    bool begin_resync = false;

    if (succeeded && completed_kind == CONTROL_REQUEST_EVENT_POLL) {
        bool recovered = event_poll_failed;
        event_poll_failed = false;
        event_poll_retry_frames = EVENT_POLL_RETRY_INITIAL_FRAMES;
        event_poll_failure_wait_frames = EVENT_POLL_RETRY_INITIAL_FRAMES;
        event_detail[0] = '\0';
        snprintf(event_batch, sizeof(event_batch), "%s", body);
        if (handle_event_batch(event_batch)) {
            redraw();
        } else if (recovered) {
            set_view_state("Event connection restored");
            redraw();
        }
    } else if (succeeded
        && completed_kind == CONTROL_REQUEST_SESSION_RESYNC) {
        if (!apply_session_snapshot(body)) {
            mark_event_poll_failure("Resync", "malformed session snapshot");
        } else {
            redraw();
        }
    } else if (succeeded) {
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Command accepted in %u ms",
            (unsigned int)(osGetTime() - control_request_started_ms)
        );
        set_view_state(control_accepted_state);
        force_event_poll();
        redraw();
    } else if (completed_kind == CONTROL_REQUEST_EVENT_POLL
        && http_status == 409) {
        begin_resync = true;
    } else if (completed_kind == CONTROL_REQUEST_EVENT_POLL
        || completed_kind == CONTROL_REQUEST_SESSION_RESYNC) {
        mark_event_poll_failure(
            completed_kind == CONTROL_REQUEST_EVENT_POLL ? "Events" : "Resync",
            request_error
        );
    } else {
        snprintf(response, sizeof(response), "%s", request_error);
        snprintf(network_detail, sizeof(network_detail), "%s", request_error);
        set_view_state(
            completed_kind == CONTROL_REQUEST_TEXT_CAPTURE
                ? "Capture error - retry A"
                : "Command failed"
        );
        redraw();
    }

    clear_control_request();
    if (begin_resync) {
        start_session_resync();
    }
}

static void poll_events_if_due(void)
{
    if (!network_ready || recording_session_active
        || control_request_kind != CONTROL_REQUEST_NONE) {
        return;
    }
    event_poll_frames++;
    if (event_poll_frames < desired_event_poll_frames()) {
        return;
    }
    event_poll_frames = 0;

    char path[160];
    int path_size = snprintf(
        path,
        sizeof(path),
        "/v1/events?sessionId=%s&after=%u&limit=%u",
        STAGE1_SESSION_ID,
        event_cursor,
        EVENT_POLL_BATCH_LIMIT
    );
    if (path_size < 0 || (size_t)path_size >= sizeof(path)) {
        mark_event_poll_failure("Events", "path exceeded bounded buffer");
        return;
    }
    if (!start_control_get(CONTROL_REQUEST_EVENT_POLL, path)) {
        mark_event_poll_failure("Events", network_detail);
    }
}

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
    swkbdSetHintText(&keyboard, "Send a prompt to the local fake agent");
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Send", true);
    swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&keyboard, SWKBD_PREDICTIVE_INPUT);

    SwkbdButton button = swkbdInputText(&keyboard, prompt, sizeof(prompt));
    *keyboard_result = swkbdGetResult(&keyboard);
    return button;
}

static void submit_text_capture(const char *text)
{
    response[0] = '\0';
    response_scroll_lines = 0;
    if (network_ready) {
        network_detail[0] = '\0';
    }

    snprintf(prompt, sizeof(prompt), "%s", text);
    set_view_state("Submitting capture...");
    redraw();
    present_frame();

    if (!network_ready) {
        snprintf(
            response,
            sizeof(response),
            "Network initialization failed: %s",
            network_detail
        );
        set_view_state("Error - retry A/X");
    } else {
        start_control_post(
            CONTROL_REQUEST_TEXT_CAPTURE,
            STAGE1_TEXT_CAPTURE_PATH,
            "text/plain; charset=utf-8",
            text,
            strlen(text),
            "Capture accepted"
        );
    }

    redraw();
}

static bool post_json_command(
    const char *path,
    const char *body,
    const char *accepted_state
)
{
    ControlRequestKind kind = strcmp(path, STAGE1_INTERRUPT_PATH) == 0
        ? CONTROL_REQUEST_INTERRUPT
        : CONTROL_REQUEST_APPROVAL;
    if (!start_control_post(
            kind,
            path,
            "application/json; charset=utf-8",
            body,
            strlen(body),
            accepted_state
        )) {
        return false;
    }
    set_view_state("Sending command...");
    redraw();
    return true;
}

static void respond_to_approval(const char *choice)
{
    if (pending_approval_id[0] == '\0') {
        return;
    }
    char path[192];
    int path_size = snprintf(
        path,
        sizeof(path),
        "/v1/sessions/%s/approvals/%s/respond",
        STAGE1_SESSION_ID,
        pending_approval_id
    );
    if (path_size < 0 || (size_t)path_size >= sizeof(path)) {
        snprintf(network_detail, sizeof(network_detail), "Approval path is too large");
        set_view_state("Approval error");
        redraw();
        return;
    }

    char body[64];
    int body_size = snprintf(body, sizeof(body), "{\"choice\":\"%s\"}", choice);
    if (body_size < 0 || (size_t)body_size >= sizeof(body)) {
        snprintf(network_detail, sizeof(network_detail), "Approval body is too large");
        set_view_state("Approval error");
        redraw();
        return;
    }
    post_json_command(path, body, "Approval sent");
}

static void interrupt_active_turn(void)
{
    if (!turn_active) {
        return;
    }
    post_json_command(STAGE1_INTERRUPT_PATH, "", "Interrupt sent");
}

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
        snprintf(response, sizeof(response), "%s", error_message);
    } else {
        snprintf(response, sizeof(response), "Microphone stream failed.");
    }
    set_view_state("Audio stream error");
    redraw();
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
        snprintf(
            response,
            sizeof(response),
            "Network initialization failed: %s",
            network_detail
        );
        set_view_state("Audio stream error");
        redraw();
        return;
    }

    if (!microphone_begin_capture(
            microphone_detail,
            sizeof(microphone_detail)
        )) {
        snprintf(response, sizeof(response), "%s", microphone_detail);
        set_view_state("Microphone error");
        redraw();
        return;
    }

    microphone_stream_size = 0;
    microphone_stop_requested = false;
    microphone_maximum_reached = false;
    microphone_network_finish_requested = false;
    microphone_link_ready = false;
    microphone_link_latency_ms = 0;
    microphone_finish_started_ms = 0;
    snprintf(
        response,
        sizeof(response),
        "Streaming signed 16-bit mono PCM to the laptop while R is held. "
        "Release R to finalize latest.wav."
    );
    recording_session_active = true;
    set_view_state("Connecting audio...");
    redraw();

    microphone_link_started_ms = osGetTime();
    if (!network_audio_stream_begin(
            THREEGENT_SERVER_HOST,
            THREEGENT_SERVER_PORT,
            STAGE1_AUDIO_CAPTURE_PATH,
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
        snprintf(response, sizeof(response), "%s", stream_error);
        set_view_state("Audio stream error");
        redraw();
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
    redraw();
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
        response[0] = '\0';
        force_event_poll();
        set_view_state(
            microphone_maximum_reached
                ? "5-minute audio accepted"
                : "Audio accepted"
        );
        microphone_maximum_reached = false;
        redraw();
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
        redraw();
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
                : "Streaming audio..."
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
    redraw();
}

static void change_scroll(bool scroll_up)
{
    size_t max_scroll = get_max_scroll();
    if (scroll_up && response_scroll_lines < max_scroll) {
        response_scroll_lines++;
    } else if (!scroll_up && response_scroll_lines > 0) {
        response_scroll_lines--;
    }
    redraw();
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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    gfxInitDefault();
    consoleInit(GFX_TOP, &top_console);
    consoleInit(GFX_BOTTOM, &bottom_console);

    network_ready = network_start(network_detail, sizeof(network_detail));
    microphone_initialize(microphone_detail, sizeof(microphone_detail));
    if (network_ready) {
        set_view_state("Warming low-latency links...");
        redraw();
        present_frame();

        u64 warmup_started_ms = osGetTime();
        if (network_prepare_connections(
                THREEGENT_SERVER_HOST,
                THREEGENT_SERVER_PORT,
                network_detail,
                sizeof(network_detail)
            )) {
            snprintf(
                network_detail,
                sizeof(network_detail),
                "2 warm links ready in %u ms",
                (unsigned int)(osGetTime() - warmup_started_ms)
            );
            set_view_state("Ready");
        } else {
            set_view_state("Server offline - retry action");
        }
    }
    force_event_poll();
    redraw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 keys_down = hidKeysDown();
        u32 keys_held = hidKeysHeld();
        u32 keys_up = hidKeysUp();

        network_pump();
        finish_control_request();

        if ((keys_down & KEY_START) != 0) {
            break;
        }

        if ((keys_down & KEY_R) != 0 && !recording_session_active) {
            if (turn_active) {
                snprintf(
                    network_detail,
                    sizeof(network_detail),
                    "Wait for or interrupt the active turn"
                );
                set_view_state("Agent is busy");
                redraw();
            } else {
                begin_microphone_capture();
            }
        }

        if (recording_session_active) {
            if (recording_session_active
                && (keys_up & KEY_R) != 0) {
                request_microphone_finish(false);
            }
            update_microphone_capture();
            present_frame();
            continue;
        }

        if ((keys_down & KEY_A) != 0) {
            if (turn_active) {
                snprintf(
                    network_detail,
                    sizeof(network_detail),
                    "Wait for or interrupt the active turn"
                );
                set_view_state("Agent is busy");
                redraw();
            } else {
                SwkbdResult keyboard_result = SWKBD_NONE;
                SwkbdButton button = read_prompt(&keyboard_result);

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
                    redraw();
                }
            }
        }

        if ((keys_down & KEY_X) != 0) {
            if (pending_approval_id[0] != '\0') {
                respond_to_approval("approve_once");
            } else if (!turn_active) {
                submit_text_capture("please request approval");
            }
        }

        if ((keys_down & KEY_B) != 0) {
            if (pending_approval_id[0] != '\0') {
                respond_to_approval("decline");
            } else {
                interrupt_active_turn();
            }
        }

        handle_scroll_input(keys_down, keys_held);
        poll_events_if_due();

        present_frame();
    }

    microphone_shutdown();
    network_stop();
    gfxExit();
    return 0;
}
