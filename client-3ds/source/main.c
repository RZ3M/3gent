#include "app_config.h"
#include "microphone.h"
#include "network.h"

#include <3ds.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define RESPONSE_WRAP_COLUMNS 48
#define RESPONSE_MAX_LINES 64
#define RESPONSE_VISIBLE_LINES 22
#define SCROLL_REPEAT_DELAY_FRAMES 18
#define SCROLL_REPEAT_INTERVAL_FRAMES 4
#define MIC_STREAM_BUFFER_CAPACITY 8192
#define MIC_STREAM_SEND_THRESHOLD 1024

static PrintConsole top_console;
static PrintConsole bottom_console;

static char prompt[THREEGENT_PROMPT_CAPACITY];
static char response[THREEGENT_RESPONSE_CAPACITY];
static char network_detail[160];
static char microphone_detail[160];
static char view_state[32] = "Ready";
static char wrapped_lines[RESPONSE_MAX_LINES][RESPONSE_WRAP_COLUMNS + 1];
static u8 microphone_stream_buffer[MIC_STREAM_BUFFER_CAPACITY];
static bool network_ready;
static bool recording_session_active;
static size_t microphone_stream_size;
static size_t response_scroll_lines;
static int scroll_repeat_direction;
static u32 scroll_repeat_frames;

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
    printf("Stage 0A-E feasibility\n");
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

    printf("A: Type + echo\n");
    printf("X: Incremental demo\n");
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

static void show_response_progress(const char *current_response, void *user_data)
{
    (void)current_response;
    (void)user_data;

    response_scroll_lines = 0;
    set_view_state("Receiving...");
    redraw();
    present_frame();
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
    swkbdSetHintText(&keyboard, "Type a short Stage 0 test message");
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Send", true);
    swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&keyboard, SWKBD_PREDICTIVE_INPUT);

    SwkbdButton button = swkbdInputText(&keyboard, prompt, sizeof(prompt));
    *keyboard_result = swkbdGetResult(&keyboard);
    return button;
}

static void run_request(const char *path, const char *success_state)
{
    u64 request_started_ms = osGetTime();
    response[0] = '\0';
    response_scroll_lines = 0;
    if (network_ready) {
        network_detail[0] = '\0';
    }

    set_view_state("Sending...");
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
    } else if (network_post_text(
                   THREEGENT_SERVER_HOST,
                   THREEGENT_SERVER_PORT,
                   path,
                   prompt,
                   response,
                   sizeof(response),
                   network_detail,
                   sizeof(network_detail),
                   show_response_progress,
                   NULL
               )) {
        snprintf(
            network_detail,
            sizeof(network_detail),
            "Request completed in %u ms",
            (unsigned int)(osGetTime() - request_started_ms)
        );
        set_view_state(success_state);
    } else {
        if (response[0] == '\0') {
            snprintf(response, sizeof(response), "%s", network_detail);
        }
        set_view_state("Error - retry A/X");
    }

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

    if (error_message != NULL && error_message[0] != '\0') {
        snprintf(response, sizeof(response), "%s", error_message);
    } else {
        snprintf(response, sizeof(response), "Microphone stream failed.");
    }
    set_view_state("Audio stream error");
    redraw();
}

static bool send_microphone_stream_buffer(bool force)
{
    if (microphone_stream_size == 0) {
        return true;
    }
    if (!force && microphone_stream_size < MIC_STREAM_SEND_THRESHOLD) {
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
    return true;
}

static bool drain_microphone_samples(void)
{
    while (true) {
        if (microphone_stream_size == sizeof(microphone_stream_buffer)
            && !send_microphone_stream_buffer(true)) {
            return false;
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

        if (!send_microphone_stream_buffer(false)) {
            return false;
        }
        if (bytes_read == 0) {
            return true;
        }
    }
}

static void begin_microphone_capture(void)
{
    u64 action_started_ms = osGetTime();
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
    snprintf(
        response,
        sizeof(response),
        "Streaming signed 16-bit mono PCM to the laptop while R is held. Release R to finalize latest.wav."
    );
    recording_session_active = true;
    set_view_state("Starting audio stream...");
    redraw();
    present_frame();

    u64 link_started_ms = osGetTime();
    if (!network_audio_stream_begin(
            THREEGENT_SERVER_HOST,
            THREEGENT_SERVER_PORT,
            "/audio/stream",
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

    snprintf(
        network_detail,
        sizeof(network_detail),
        "Audio ready in %u ms (link %u ms)",
        (unsigned int)(osGetTime() - action_started_ms),
        (unsigned int)(osGetTime() - link_started_ms)
    );
    set_view_state("Streaming audio...");
    redraw();
}

static void update_microphone_capture(void)
{
    if (!drain_microphone_samples()) {
        const char *detail = microphone_detail[0] != '\0'
            ? microphone_detail
            : network_detail;
        stop_failed_microphone_stream(detail);
        return;
    }

    if (microphone_capture_is_full()) {
        set_view_state("5-minute limit reached");
    }
    redraw();
}

static void finish_microphone_capture(bool maximum_reached)
{
    if (!microphone_finish_capture(
            microphone_detail,
            sizeof(microphone_detail)
        )) {
        stop_failed_microphone_stream(microphone_detail);
        return;
    }

    if (!drain_microphone_samples()
        || !send_microphone_stream_buffer(true)) {
        const char *detail = microphone_detail[0] != '\0'
            ? microphone_detail
            : network_detail;
        stop_failed_microphone_stream(detail);
        return;
    }

    size_t total_pcm_size = microphone_total_pcm_size();
    if (total_pcm_size == 0) {
        stop_failed_microphone_stream("No microphone samples were captured.");
        return;
    }

    set_view_state("Finalizing audio...");
    redraw();
    present_frame();
    response[0] = '\0';
    if (!network_audio_stream_finish(
            response,
            sizeof(response),
            network_detail,
            sizeof(network_detail)
        )) {
        stop_failed_microphone_stream(network_detail);
        return;
    }

    recording_session_active = false;
    microphone_stream_size = 0;
    snprintf(
        microphone_detail,
        sizeof(microphone_detail),
        "Held %u ms; captured %u ms (%u bytes, %u offset changes)",
        microphone_wall_duration_ms(),
        microphone_duration_ms(),
        (unsigned int)total_pcm_size,
        microphone_offset_change_count()
    );
    network_detail[0] = '\0';
    set_view_state(
        maximum_reached ? "5-minute stream complete" : "Audio stream complete"
    );
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
    redraw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 keys_down = hidKeysDown();
        u32 keys_held = hidKeysHeld();
        u32 keys_up = hidKeysUp();

        if ((keys_down & KEY_START) != 0) {
            break;
        }

        if ((keys_down & KEY_R) != 0 && !recording_session_active) {
            begin_microphone_capture();
        }

        if (recording_session_active) {
            update_microphone_capture();
            if (recording_session_active
                && (keys_up & KEY_R) != 0) {
                finish_microphone_capture(false);
            } else if (recording_session_active
                && microphone_capture_is_full()) {
                finish_microphone_capture(true);
            }
            present_frame();
            continue;
        }

        if ((keys_down & KEY_A) != 0) {
            SwkbdResult keyboard_result = SWKBD_NONE;
            SwkbdButton button = read_prompt(&keyboard_result);

            if (button == SWKBD_BUTTON_RIGHT) {
                run_request("/echo", "Echo complete");
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

        if ((keys_down & KEY_X) != 0) {
            if (prompt[0] == '\0') {
                snprintf(prompt, sizeof(prompt), "incremental output demo");
            }
            run_request("/stream", "Stream complete");
        }

        handle_scroll_input(keys_down, keys_held);

        present_frame();
    }

    microphone_shutdown();
    network_stop();
    gfxExit();
    return 0;
}
