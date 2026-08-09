/*
 * Renders the handheld interface states to SVG on the host.
 *
 * It links the real `client-3ds/source/ui.c` against the recording backend in
 * `svg_backend.c`, so what you see is produced by the same layout code that
 * ships on the device.
 */
#include "svg_backend.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_DIRECTORY "out"

static const char *sample_labels[6] = {
    "3gent - wire the citro2d renderer",
    "bridge - bound the replay window",
    "protocol - version the capture envelope",
    "docs - rewrite the hardware checklist",
    "relay - reject plaintext by default",
    "client - retry one unacked command",
};

static const char *idle_response =
    "I looked at the push transport and the reconnect path is fine, but the "
    "cursor is written before the event is applied. If the frame is dropped "
    "between those two steps the client asks for the next sequence and the "
    "bridge never replays it.\n"
    "\n"
    "Moving the cursor write after the handler fixes it, and the resync path "
    "already covers the case where history has expired.";

static const char *working_response =
    "Reading client-3ds/source/network.c to confirm how the control socket "
    "reports a half-open connection.\n"
    "\n"
    "The heartbeat is sent every three seconds and the bridge drops the "
    "session after twelve, so a stalled write should surface within one "
    "reconnect window. Patching the cursor order now";

#define MAX_DOCUMENTS 24
static char document_captions[MAX_DOCUMENTS][96];
static char *document_markup[MAX_DOCUMENTS];
static size_t document_count;

static void write_document(const char *name, const char *caption)
{
    char *markup = NULL;
    size_t markup_size = 0;
    FILE *file = open_memstream(&markup, &markup_size);
    if (file == NULL) {
        fprintf(stderr, "could not compose %s\n", name);
        return;
    }

    fprintf(
        file,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"440\" height=\"548\""
        " viewBox=\"0 0 440 548\">\n"
        "  <defs>\n%s  </defs>\n"
        "  <rect x=\"0\" y=\"0\" width=\"440\" height=\"548\" rx=\"18\""
        " fill=\"rgb(26,29,36)\"/>\n"
        "  <text x=\"220\" y=\"17\" text-anchor=\"middle\""
        " font-family=\"'Avenir Next', system-ui, sans-serif\" font-size=\"11\""
        " fill=\"rgb(120,130,150)\">%s</text>\n"
        "  <g transform=\"translate(20,26)\">\n"
        "    <rect x=\"-2\" y=\"-2\" width=\"404\" height=\"244\" rx=\"3\""
        " fill=\"rgb(8,9,12)\"/>\n"
        "%s"
        "  </g>\n"
        "  <g transform=\"translate(60,290)\">\n"
        "    <rect x=\"-2\" y=\"-2\" width=\"324\" height=\"244\" rx=\"3\""
        " fill=\"rgb(8,9,12)\"/>\n"
        "%s"
        "  </g>\n"
        "</svg>\n",
        preview_defs_svg(),
        caption,
        preview_screen_svg(0),
        preview_screen_svg(1)
    );
    fclose(file);

    char path[256];
    snprintf(path, sizeof(path), OUTPUT_DIRECTORY "/%s.svg", name);
    FILE *output = fopen(path, "w");
    if (output == NULL) {
        fprintf(stderr, "could not write %s\n", path);
        free(markup);
        return;
    }
    fwrite(markup, 1, markup_size, output);
    fclose(output);
    printf("wrote %s\n", path);

    if (document_count < MAX_DOCUMENTS) {
        snprintf(document_captions[document_count], 96, "%s", caption);
        document_markup[document_count] = markup;
        document_count++;
    } else {
        free(markup);
    }
}

static void write_index(void)
{
    FILE *file = fopen(OUTPUT_DIRECTORY "/index.html", "w");
    if (file == NULL) {
        return;
    }
    fprintf(
        file,
        "<!doctype html>\n<meta charset=\"utf-8\">\n"
        "<title>3gent handheld interface</title>\n"
        "<style>\n"
        "body{margin:0;padding:32px;background:#0d0f14;"
        "font:14px/1.5 'Avenir Next',system-ui,sans-serif;color:#c8d0e0}\n"
        "h1{font-size:18px;font-weight:600;margin:0 0 4px}\n"
        "p.lede{color:#7c879d;margin:0 0 28px}\n"
        ".grid{display:grid;gap:24px;"
        "grid-template-columns:repeat(auto-fill,minmax(400px,1fr))}\n"
        "figure{margin:0}\nfigure svg{width:100%%;height:auto;display:block}\n"
        "figcaption{color:#7c879d;font-size:12px;margin-top:8px}\n"
        "</style>\n"
        "<h1>3gent handheld interface</h1>\n"
        "<p class=\"lede\">Rendered by the shipping citro2d layout code in "
        "client-3ds/source/ui.c. Glyph advances are approximated off-device.</p>\n"
        "<div class=\"grid\">\n"
    );
    for (size_t index = 0; index < document_count; index++) {
        fprintf(file, "<figure>\n%s<figcaption>%s</figcaption></figure>\n",
            document_markup[index], document_captions[index]);
    }
    fprintf(file, "</div>\n");
    fclose(file);
    printf("wrote " OUTPUT_DIRECTORY "/index.html\n");
}

static const char *menu_labels[5] = {
    "Connect",
    "Pair with a QR code",
    "Enter a pairing code",
    "Forget this machine",
    "Exit",
};

static const char *menu_hints[5] = {
    "jm-mbp at 192.168.1.42:8080",
    "Scan the code your bridge prints",
    "Type the address and code by hand",
    "Delete the device key saved on the SD card",
    "Close 3gent",
};

static const bool menu_paired[5] = { true, true, true, true, true };
static const bool menu_unpaired[5] = { true, true, true, false, true };

static void base_model(UiModel *model)
{
    memset(model, 0, sizeof(*model));
    model->version = "0.8.0-pairing";
    model->session_label = "3gent - wire the citro2d renderer";
    model->server_host = "192.168.1.2";
    model->server_port = 8080;
    model->network_ready = true;
    model->audio_warm = true;
    model->microphone_ready = true;
    model->link_state = "ready";
    model->event_cursor = 148;
    model->agent_state = "idle";
    model->view_state = "Ready";
    model->detail = "Audio link ready in 214 ms";
    model->detail_secondary = "Events: pushed link ready";
    model->prompt = "";
    model->response = "";
    model->transcript = "";
    model->approval_summary = "";
    model->photo_caption = "";
    model->record_max_ms = 300000;
    model->photo_progress_percent = UI_PHOTO_PROGRESS_NONE;
    model->session_labels = sample_labels;
    model->sessions_status = "";
    model->menu_labels = menu_labels;
    model->menu_hints = menu_hints;
    model->menu_enabled = menu_paired;
    model->menu_count = 5;
    model->paired = true;
    model->paired_bridge = "jm-mbp";
    model->paired_endpoint = "192.168.1.42  port 8080  push 8081";
    model->paired_since = "paired 2026-08-09  key dev_5f2ac91b0e77d4a3";
    model->pairing_message = "";
    model->pairing_bridge = "";
    model->screen = UI_SCREEN_MAIN;
}

int main(void)
{
    char error[96];
    if (!ui_initialize(error, sizeof(error))) {
        fprintf(stderr, "ui_initialize failed: %s\n", error);
        return 1;
    }

    UiModel model;

    /* Start screen, already paired */
    base_model(&model);
    model.screen = UI_SCREEN_HOME;
    model.agent_state = "";
    model.view_state = "Ready to connect";
    model.detail = "sdmc:/3ds/3gent/pairing.cfg";
    model.detail_secondary = "";
    model.link_state = "idle";
    model.server_host = "192.168.1.42";
    model.menu_selected = 0;
    preview_set_time(400);
    ui_render(&model);
    write_document("01-home-paired", "start / a machine is already paired");

    /* Start screen, nothing paired yet */
    base_model(&model);
    model.screen = UI_SCREEN_HOME;
    model.agent_state = "";
    model.view_state = "No machine paired";
    model.detail = "Pair with a machine, or use the built-in development host";
    model.detail_secondary = "";
    model.link_state = "idle";
    model.paired = false;
    model.paired_since = "";
    model.menu_enabled = menu_unpaired;
    model.menu_hints = (const char *const []){
        "development host 192.168.1.2:8080",
        menu_hints[1],
        menu_hints[2],
        menu_hints[3],
        menu_hints[4],
    };
    model.menu_selected = 1;
    preview_set_time(700);
    ui_render(&model);
    write_document("02-home-unpaired", "start / nothing paired yet");

    /* Pairing viewfinder */
    base_model(&model);
    model.screen = UI_SCREEN_PAIRING;
    model.view_state = "Scanning for a QR code";
    model.detail = "Outer camera, 400x240 RGB565";
    model.detail_secondary = "";
    model.pairing_phase = UI_PAIRING_AIMING;
    model.pairing_preview_ready = true;
    model.pairing_frames_examined = 14;
    model.pairing_message = "Point the outer camera at the QR code";
    static u8 viewfinder[400 * 240 * 2];
    ui_photo_preview_set(viewfinder, 400, 240);
    preview_set_time(1000);
    ui_render(&model);
    write_document("03-pairing-aiming", "pairing / aiming at the bridge QR code");

    /* Pairing exchange */
    base_model(&model);
    model.screen = UI_SCREEN_PAIRING;
    model.view_state = "Pairing...";
    model.detail = "POST /v1/pair";
    model.detail_secondary = "";
    model.pairing_phase = UI_PAIRING_EXCHANGING;
    model.pairing_preview_ready = true;
    model.pairing_message = "Asking jm-mbp to pair...";
    model.pairing_bridge = "jm-mbp at 192.168.1.42:8080";
    preview_set_time(1300);
    ui_render(&model);
    write_document("04-pairing-exchange", "pairing / exchanging the one-time code");

    /* Pairing failure */
    base_model(&model);
    model.screen = UI_SCREEN_PAIRING;
    model.view_state = "Not paired";
    model.detail = "HTTP 403";
    model.detail_secondary = "";
    model.pairing_phase = UI_PAIRING_FAILED;
    model.pairing_preview_ready = true;
    model.pairing_message = "pairing code is not valid";
    preview_set_time(1600);
    ui_render(&model);
    write_document("05-pairing-failed", "pairing / the bridge refused the code");
    ui_photo_preview_clear();

    /* Boot */
    base_model(&model);
    model.screen = UI_SCREEN_BOOT;
    model.view_state = "Warming low-latency links...";
    model.detail = "Connecting to 192.168.1.2:8080";
    model.detail_secondary = "";
    model.link_state = "connecting";
    preview_set_time(1200);
    ui_render(&model);
    write_document("06-boot", "boot / warming the low-latency links");

    /* Task chooser */
    base_model(&model);
    model.screen = UI_SCREEN_SESSIONS;
    model.agent_state = "connecting";
    model.session_label = "";
    model.view_state = "Choose a task";
    model.detail = "6 recent Codex tasks";
    model.detail_secondary = "";
    model.session_count = 6;
    model.session_selected = 1;
    model.sessions_status = "A resumes the highlighted task, X starts a new one";
    preview_set_time(2400);
    ui_render(&model);
    write_document("07-sessions", "task chooser / six recent Codex tasks");

    /* Task chooser, discovery failed */
    base_model(&model);
    model.screen = UI_SCREEN_SESSIONS;
    model.agent_state = "connecting";
    model.session_label = "";
    model.view_state = "Task discovery failed";
    model.detail = "Session request failed: connection refused";
    model.detail_secondary = "";
    model.link_state = "retrying";
    model.sessions_retryable = true;
    model.sessions_status = "Session request failed: connection refused";
    preview_set_time(3000);
    ui_render(&model);
    write_document("08-sessions-error", "task chooser / bridge unreachable");

    /* Idle with a completed response */
    base_model(&model);
    model.prompt = "why does the push cursor skip an event after a reconnect?";
    model.response = idle_response;
    model.view_state = "Response complete";
    model.detail = "Command accepted in 38 ms";
    model.diff_known = true;
    model.diff_files = 2;
    model.diff_additions = 31;
    model.diff_deletions = 12;
    preview_set_time(4200);
    ui_render(&model);
    write_document("09-idle", "main / completed turn with a diff summary");

    /* Streaming */
    base_model(&model);
    model.prompt = "fix the cursor ordering and add a regression test";
    model.response = working_response;
    model.agent_state = "working";
    model.turn_active = true;
    model.view_state = "Receiving response...";
    model.detail = "Command accepted in 41 ms";
    model.detail_secondary = "Events: pushed link ready";
    preview_set_time(5600);
    ui_render(&model);
    write_document("10-working", "main / agent working, response streaming");

    /* Recording. Several frames prime the scrolling level history. */
    base_model(&model);
    model.response = "";
    model.recording = true;
    model.view_state = "Recording...";
    model.detail = "PCM 7180 ms | MICU sampling | no new data 12 ms";
    model.detail_secondary = "Audio link ready in 96 ms";
    static const unsigned int levels[28] = {
        12, 34, 58, 71, 44, 22, 61, 83, 92, 74, 41, 26, 18, 39,
        67, 88, 95, 76, 52, 33, 47, 69, 81, 58, 36, 24, 44, 62,
    };
    for (unsigned int frame = 0; frame < 56; frame++) {
        model.record_ms = 7000 + frame * 30;
        model.record_level_percent = levels[(frame / 2) % 28];
        preview_set_time(9000 + frame * 16);
        ui_render(&model);
    }
    write_document("11-recording", "main / push-to-talk with a live level trace");

    /* Transcript review */
    base_model(&model);
    model.transcript_ready = true;
    model.transcript =
        "Check whether the reconnect path replays the event we dropped, and "
        "add a test that forces a half-open socket during a turn.";
    model.response = model.transcript;
    model.view_state = "Transcript ready";
    model.detail = "Held 7420 ms; captured 7380 ms (241664 bytes)";
    preview_set_time(11000);
    ui_render(&model);
    write_document("12-transcript", "main / review the transcript before sending");

    /* Approval */
    base_model(&model);
    model.prompt = "fix the cursor ordering and add a regression test";
    model.response = working_response;
    model.agent_state = "waiting_for_user";
    model.turn_active = true;
    model.approval_pending = true;
    model.approval_summary =
        "Run `npm test -- push-server` in /Users/jm/Projects/3gent/bridge";
    model.view_state = "Approval required";
    model.detail = "Command accepted in 44 ms";
    preview_set_time(12600);
    ui_render(&model);
    write_document("13-approval", "main / approval required");

    /* Photo review */
    base_model(&model);
    model.screen = UI_SCREEN_PHOTO;
    model.view_state = "Review the photo";
    model.detail = "Outer camera, 400x240 RGB565";
    model.detail_secondary = "";
    model.photo_caption = "Attach this shot to your next prompt";
    static u8 photo[400 * 240 * 2];
    ui_photo_preview_set(photo, 400, 240);
    preview_set_time(14000);
    ui_render(&model);
    write_document("14-photo", "camera / review before attaching");
    ui_photo_preview_clear();

    write_index();
    ui_shutdown();
    return 0;
}
