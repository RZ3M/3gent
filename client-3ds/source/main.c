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

static PrintConsole top_console;
static PrintConsole bottom_console;

static char prompt[THREEGENT_PROMPT_CAPACITY];
static char response[THREEGENT_RESPONSE_CAPACITY];
static char network_detail[160];
static char microphone_detail[160];
static char view_state[32] = "Ready";
static char wrapped_lines[RESPONSE_MAX_LINES][RESPONSE_WRAP_COLUMNS + 1];
static bool network_ready;
static bool recording_session_active;
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
    printf("R: Hold to record (10 sec max)\n");
    printf("Y: Upload last recording\n");
    printf("UP/DOWN: Scroll response\n");
    printf("START: Exit\n\n");
    printf("Server: %s:%u\n", THREEGENT_SERVER_HOST, (unsigned int)THREEGENT_SERVER_PORT);
    printf(
        "Network: %s\n",
        network_ready ? "ready" : "unavailable"
    );
    printf(
        "Mic: %s (PCM16 mono, 16364 Hz)\n",
        microphone_is_ready() ? "ready" : "unavailable"
    );

    if (recording_session_active) {
        unsigned int duration_ms = microphone_duration_ms();
        unsigned int level = microphone_level_percent();
        unsigned int level_marks = level / 10;
        if (level_marks > 10) {
            level_marks = 10;
        }

        printf(
            "Recording: %u.%02u / %u.00 sec\n",
            duration_ms / 1000,
            (duration_ms % 1000) / 10,
            THREEGENT_MIC_MAX_SECONDS
        );
        printf("Level: [");
        for (unsigned int mark = 0; mark < 10; mark++) {
            printf("%c", mark < level_marks ? '#' : '.');
        }
        printf("] %u%%\n", level);
    } else if (microphone_has_capture()) {
        unsigned int duration_ms = microphone_duration_ms();
        printf(
            "Last audio: %u.%02u sec, %u bytes\n",
            duration_ms / 1000,
            (duration_ms % 1000) / 10,
            (unsigned int)microphone_wav_size()
        );
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
    response[0] = '\0';
    response_scroll_lines = 0;
    if (network_ready) {
        network_detail[0] = '\0';
    }

    set_view_state("Connecting...");
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
        network_detail[0] = '\0';
        set_view_state(success_state);
    } else {
        if (response[0] == '\0') {
            snprintf(response, sizeof(response), "%s", network_detail);
        }
        set_view_state("Error - retry A/X");
    }

    redraw();
}

static void upload_microphone_capture(void)
{
    response[0] = '\0';
    response_scroll_lines = 0;
    if (network_ready) {
        network_detail[0] = '\0';
    }

    if (!microphone_has_capture()) {
        snprintf(response, sizeof(response), "No completed recording to upload.");
        set_view_state("No audio captured");
        redraw();
        return;
    }

    set_view_state("Uploading audio...");
    redraw();
    present_frame();

    if (!network_ready) {
        snprintf(
            response,
            sizeof(response),
            "Network initialization failed: %s",
            network_detail
        );
        set_view_state("Upload error - retry Y");
    } else if (network_post_bytes(
                   THREEGENT_SERVER_HOST,
                   THREEGENT_SERVER_PORT,
                   "/audio",
                   "audio/wav",
                   microphone_wav_data(),
                   microphone_wav_size(),
                   response,
                   sizeof(response),
                   network_detail,
                   sizeof(network_detail),
                   NULL,
                   NULL
               )) {
        network_detail[0] = '\0';
        set_view_state("Audio upload complete");
    } else {
        if (response[0] == '\0') {
            snprintf(response, sizeof(response), "%s", network_detail);
        }
        set_view_state("Upload error - retry Y");
    }

    redraw();
}

static void begin_microphone_capture(void)
{
    microphone_detail[0] = '\0';
    if (!microphone_begin_capture(
            microphone_detail,
            sizeof(microphone_detail)
        )) {
        snprintf(response, sizeof(response), "%s", microphone_detail);
        set_view_state("Microphone error");
        redraw();
        return;
    }

    response_scroll_lines = 0;
    snprintf(
        response,
        sizeof(response),
        "Recording signed 16-bit mono PCM. Speak while holding R, then release to upload the WAV file."
    );
    recording_session_active = true;
    set_view_state("Recording...");
    redraw();
}

static void update_microphone_capture(void)
{
    if (!microphone_is_sampling()) {
        return;
    }

    if (!microphone_poll_capture(
            microphone_detail,
            sizeof(microphone_detail)
        )) {
        snprintf(response, sizeof(response), "%s", microphone_detail);
        recording_session_active = false;
        set_view_state("Microphone error");
        redraw();
        return;
    }

    if (microphone_capture_is_full()) {
        set_view_state("Full - release R");
    }
    redraw();
}

static void finish_microphone_capture(void)
{
    if (!microphone_finish_capture(
            microphone_detail,
            sizeof(microphone_detail)
        )) {
        snprintf(response, sizeof(response), "%s", microphone_detail);
        recording_session_active = false;
        set_view_state("Microphone error");
        redraw();
        return;
    }

    recording_session_active = false;
    snprintf(
        microphone_detail,
        sizeof(microphone_detail),
        "Captured %u PCM bytes in %u ms",
        (unsigned int)microphone_pcm_size(),
        microphone_duration_ms()
    );
    set_view_state("Capture complete");
    redraw();
    present_frame();
    upload_microphone_capture();
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
            if ((keys_up & KEY_R) != 0) {
                finish_microphone_capture();
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

        if ((keys_down & KEY_Y) != 0) {
            upload_microphone_capture();
        }

        handle_scroll_input(keys_down, keys_held);

        present_frame();
    }

    microphone_shutdown();
    network_stop();
    gfxExit();
    return 0;
}
