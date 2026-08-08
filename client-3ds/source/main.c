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
#define STAGE1_SESSION_ID "ses_fake_local"
#define STAGE1_AUDIO_CAPTURE_PATH \
    "/v1/sessions/" STAGE1_SESSION_ID "/captures/audio"

typedef enum {
    PUSH_COMMAND_NONE = 0,
    PUSH_COMMAND_TEXT_CAPTURE,
    PUSH_COMMAND_APPROVAL,
    PUSH_COMMAND_INTERRUPT,
} PushCommandKind;

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
static unsigned int event_cursor;
static bool turn_active;
static PushCommandKind push_command_kind;
static u64 push_command_started_ms;

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
    printf("Stage 1.5 | Pushed fake agent\n");
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

    printf("A: Type + send to agent\n");
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
    printf(
        "Server: %s:%u/%u\n",
        THREEGENT_SERVER_HOST,
        (unsigned int)THREEGENT_SERVER_PORT,
        (unsigned int)THREEGENT_PUSH_PORT
    );
    printf(
        "Network: %s | audio warm: %u\n",
        network_ready ? "ready" : "unavailable",
        network_warm_connection_count() > 0 ? 1U : 0U
    );
    printf("Control push: %s\n", network_push_state());
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
    }

    response[0] = '\0';
    append_response("[Event history changed; session state resynchronized.]\n");
    if (pending_approval_id[0] != '\0') {
        append_response("Approval is still pending. X: approve once | B: decline\n");
    }
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
    bool changed = strcmp(previous_state, state) != 0;
    if (changed) {
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
        changed = true;
    }
    if (changed) {
        redraw();
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

static bool restart_push_link(void)
{
    if (!network_ready) {
        return false;
    }
    if (!network_push_start(
            THREEGENT_SERVER_HOST,
            THREEGENT_PUSH_PORT,
            STAGE1_SESSION_ID,
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
            snprintf(response, sizeof(response), "%s", network_detail);
            set_view_state("Capture error - retry A");
        }
    }

    redraw();
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
        redraw();
        return;
    }
    push_command_kind = PUSH_COMMAND_APPROVAL;
    push_command_started_ms = osGetTime();
    set_view_state("Sending approval...");
    redraw();
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
        redraw();
        return;
    }
    push_command_kind = PUSH_COMMAND_INTERRUPT;
    push_command_started_ms = osGetTime();
    set_view_state("Sending interrupt...");
    redraw();
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
                "Audio link ready in %u ms",
                (unsigned int)(osGetTime() - warmup_started_ms)
            );
            set_view_state("Ready");
        } else {
            set_view_state("Server offline - retry action");
        }
        restart_push_link();
    }
    redraw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 keys_down = hidKeysDown();
        u32 keys_held = hidKeysHeld();
        u32 keys_up = hidKeysUp();

        network_pump();
        update_push_link();

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
            if (turn_active || push_command_kind != PUSH_COMMAND_NONE) {
                snprintf(
                    network_detail,
                    sizeof(network_detail),
                    "%s",
                    turn_active
                        ? "Wait for or interrupt the active turn"
                        : "Wait for the queued command acknowledgement"
                );
                set_view_state("Agent is busy");
                redraw();
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

        present_frame();
    }

    microphone_shutdown();
    network_stop();
    gfxExit();
    return 0;
}
