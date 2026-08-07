#include "app_config.h"
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
static char view_state[32] = "Ready";
static char wrapped_lines[RESPONSE_MAX_LINES][RESPONSE_WRAP_COLUMNS + 1];
static bool network_ready;
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
    printf("Stage 0D incremental output\n");
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
    printf("UP/DOWN: Scroll response\n");
    printf("START: Exit\n\n");
    printf("Server: %s:%u\n", THREEGENT_SERVER_HOST, (unsigned int)THREEGENT_SERVER_PORT);
    printf(
        "Network: %s\n",
        network_ready ? "ready" : "unavailable"
    );

    if (response_scroll_lines == 0) {
        printf("Scroll: latest\n");
    } else {
        printf("Scroll: %u lines back\n", (unsigned int)response_scroll_lines);
    }

    if (network_detail[0] != '\0') {
        printf("%s\n", network_detail);
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
    redraw();

    while (aptMainLoop()) {
        hidScanInput();
        u32 keys_down = hidKeysDown();
        u32 keys_held = hidKeysHeld();

        if ((keys_down & KEY_START) != 0) {
            break;
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

    network_stop();
    gfxExit();
    return 0;
}
