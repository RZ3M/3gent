#include "app_config.h"
#include "network.h"

#include <3ds.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static PrintConsole top_console;
static PrintConsole bottom_console;

static void draw_top(
    const char *state,
    const char *prompt,
    const char *response
)
{
    consoleSelect(&top_console);
    consoleClear();

    printf("3gent\n");
    printf("Stage 0A-C | %s\n", THREEGENT_APP_VERSION);
    printf("----------------------------------------\n");
    printf("State: %s\n\n", state);

    if (prompt[0] != '\0') {
        printf("You typed:\n%s\n\n", prompt);
    }

    if (response[0] != '\0') {
        printf("Server response:\n%s\n", response);
    }
}

static void draw_bottom(bool network_ready, const char *network_detail)
{
    consoleSelect(&bottom_console);
    consoleClear();

    printf("A: Type and send\n");
    printf("START: Exit\n\n");
    printf("Development server:\n");
    printf(
        "http://%s:%u/echo\n\n",
        THREEGENT_SERVER_HOST,
        (unsigned int)THREEGENT_SERVER_PORT
    );
    printf(
        "Network service: %s\n",
        network_ready ? "ready" : "unavailable"
    );

    if (network_detail[0] != '\0') {
        printf("%s\n", network_detail);
    }
}

static void present_frame(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static SwkbdButton read_prompt(
    char *prompt,
    size_t prompt_capacity,
    SwkbdResult *keyboard_result
)
{
    SwkbdState keyboard;
    swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, (int)prompt_capacity - 1);
    swkbdSetInitialText(&keyboard, prompt);
    swkbdSetHintText(&keyboard, "Type a short Stage 0 test message");
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Send", true);
    swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&keyboard, SWKBD_PREDICTIVE_INPUT);
    SwkbdButton button = swkbdInputText(&keyboard, prompt, prompt_capacity);
    *keyboard_result = swkbdGetResult(&keyboard);
    return button;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char prompt[THREEGENT_PROMPT_CAPACITY] = {0};
    char response[THREEGENT_RESPONSE_CAPACITY] = {0};
    char network_detail[160] = {0};

    gfxInitDefault();
    consoleInit(GFX_TOP, &top_console);
    consoleInit(GFX_BOTTOM, &bottom_console);

    bool network_ready = network_start(
        network_detail,
        sizeof(network_detail)
    );

    draw_top("Ready", prompt, response);
    draw_bottom(network_ready, network_detail);

    while (aptMainLoop()) {
        hidScanInput();
        u32 keys_down = hidKeysDown();

        if ((keys_down & KEY_START) != 0) {
            break;
        }

        if ((keys_down & KEY_A) != 0) {
            SwkbdResult keyboard_result = SWKBD_NONE;
            SwkbdButton button = read_prompt(
                prompt,
                sizeof(prompt),
                &keyboard_result
            );

            if (button == SWKBD_BUTTON_RIGHT) {
                response[0] = '\0';
                if (network_ready) {
                    network_detail[0] = '\0';
                }
                draw_top("Connecting...", prompt, response);
                draw_bottom(network_ready, network_detail);
                present_frame();

                if (!network_ready) {
                    snprintf(
                        response,
                        sizeof(response),
                        "Network initialization failed: %s",
                        network_detail
                    );
                    draw_top("Error", prompt, response);
                } else if (network_echo(
                               THREEGENT_SERVER_HOST,
                               THREEGENT_SERVER_PORT,
                               prompt,
                               response,
                               sizeof(response),
                               network_detail,
                               sizeof(network_detail)
                           )) {
                    draw_top("Success", prompt, response);
                } else {
                    snprintf(response, sizeof(response), "%s", network_detail);
                    draw_top("Error", prompt, response);
                }
                draw_bottom(network_ready, network_detail);
            } else {
                snprintf(
                    network_detail,
                    sizeof(network_detail),
                    "Keyboard cancelled (result %d)",
                    (int)keyboard_result
                );
                draw_top("Input cancelled", prompt, response);
                draw_bottom(network_ready, network_detail);
            }
        }

        present_frame();
    }

    network_stop();
    gfxExit();
    return 0;
}
