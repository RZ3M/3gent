#include "ui.h"

#include <citro2d.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- theme -- */

#define UI_INK          C2D_Color32(0xE9, 0xED, 0xF7, 0xFF)
#define UI_INK_DIM      C2D_Color32(0x93, 0xA0, 0xBA, 0xFF)
#define UI_INK_FAINT    C2D_Color32(0x5D, 0x68, 0x83, 0xFF)
#define UI_BG0          C2D_Color32(0x0B, 0x0E, 0x15, 0xFF)
#define UI_BG1          C2D_Color32(0x10, 0x14, 0x1E, 0xFF)
#define UI_BG2          C2D_Color32(0x18, 0x1D, 0x2B, 0xFF)
#define UI_LINE         C2D_Color32(0x25, 0x2D, 0x40, 0xFF)
#define UI_MINT         C2D_Color32(0x4F, 0xE0, 0xA6, 0xFF)
#define UI_AMBER        C2D_Color32(0xFF, 0xC1, 0x5A, 0xFF)
#define UI_CORAL        C2D_Color32(0xFF, 0x8A, 0x62, 0xFF)
#define UI_ROSE         C2D_Color32(0xFF, 0x5C, 0x79, 0xFF)
#define UI_AZURE        C2D_Color32(0x74, 0xA8, 0xFF, 0xFF)
#define UI_VIOLET       C2D_Color32(0xA9, 0x8B, 0xFF, 0xFF)

/* The system font is authored on a 30 px body with the baseline at 25 px. */
#define UI_FONT_BODY_PX     30.0f

#define UI_SCALE_WORDMARK   0.62f
#define UI_SCALE_HEAD       0.54f
#define UI_SCALE_BODY       0.46f
#define UI_SCALE_LABEL      0.42f
#define UI_SCALE_MICRO      0.37f

#define UI_BODY_LINE_H      14.0f

#define UI_TOP_W            400.0f
#define UI_BOT_W            320.0f
#define UI_SCREEN_H         240.0f

#define UI_HEADER_H         30.0f
#define UI_FOOTER_Y         216.0f
#define UI_PAD              12.0f
#define UI_BODY_TEXT_BOTTOM 212.0f
#define UI_BODY_W           (UI_TOP_W - 2.0f * UI_PAD - 8.0f)

#define UI_CHAMFER          3.0f

/* ------------------------------------------------------------- internals -- */

#define UI_TEXT_GLYPH_CAPACITY 3072
#define UI_SCRATCH_CAPACITY    512
#define UI_WRAP_SLOT_RESPONSE  0
#define UI_WRAP_SLOT_CARD      1
#define UI_WRAP_SLOTS          2
#define UI_WRAP_MAX_LINES      192
#define UI_WAVE_BARS           28

typedef struct {
    unsigned int offset;
    unsigned int length;
} UiWrapLine;

typedef struct {
    const char *source;
    size_t source_length;
    u32 source_hash;
    float width;
    float scale;
    size_t line_count;
    UiWrapLine lines[UI_WRAP_MAX_LINES];
} UiWrapCache;

static C3D_RenderTarget *top_target;
static C3D_RenderTarget *bottom_target;
static C2D_TextBuf text_buffer;
static char scratch[UI_SCRATCH_CAPACITY];
static float ascii_advance[0x60];
static float fallback_advance;
static UiWrapCache wrap_caches[UI_WRAP_SLOTS];
static u64 frame_time_ms;

static C3D_Tex photo_texture;
static Tex3DS_SubTexture photo_subtexture;
static C2D_Image photo_image;
static bool photo_texture_ready;
static bool texture_v_axis_flipped;

static u8 wave_levels[UI_WAVE_BARS];
static unsigned int wave_head;
static unsigned int wave_tick;
static bool wave_was_recording;

static inline u32 ui_alpha(u32 color, u8 alpha)
{
    return (color & 0x00FFFFFFu) | ((u32)alpha << 24);
}

/*
 * Opaque tint. `ui_panel_outlined` lays a filled border panel underneath the
 * surface, so a translucent fill would blend against the border rather than
 * the page. Anything drawn on top of another panel mixes here instead.
 */
static u32 ui_blend(u32 color, u8 amount, u32 backdrop)
{
    const float weight = (float)amount / 255.0f;
    u32 result = 0xFF000000u;
    for (unsigned int shift = 0; shift <= 16u; shift += 8u) {
        const float from = (float)((backdrop >> shift) & 0xFFu);
        const float to = (float)((color >> shift) & 0xFFu);
        const u32 mixed = (u32)(from + (to - from) * weight + 0.5f);
        result |= (mixed & 0xFFu) << shift;
    }
    return result;
}

/* ------------------------------------------------------------ primitives -- */

static void ui_fill(float x, float y, float width, float height, u32 color)
{
    C2D_DrawRectSolid(x, y, 0.0f, width, height, color);
}

static void ui_fill_vertical(
    float x,
    float y,
    float width,
    float height,
    u32 top,
    u32 bottom
)
{
    C2D_DrawRectangle(x, y, 0.0f, width, height, top, top, bottom, bottom);
}

static void ui_fill_horizontal(
    float x,
    float y,
    float width,
    float height,
    u32 left,
    u32 right
)
{
    C2D_DrawRectangle(x, y, 0.0f, width, height, left, right, left, right);
}

/*
 * Panels are chamfered rather than rounded. Bevels read as deliberate at 3DS
 * pixel density, and they avoid citro2d's expensive circle-mode state change
 * on every surface.
 */
static void ui_panel(
    float x,
    float y,
    float width,
    float height,
    float chamfer,
    u32 color
)
{
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    if (chamfer * 2.0f > width || chamfer * 2.0f > height) {
        chamfer = 0.0f;
    }
    if (chamfer <= 0.0f) {
        ui_fill(x, y, width, height, color);
        return;
    }

    ui_fill(x, y + chamfer, width, height - 2.0f * chamfer, color);
    ui_fill(x + chamfer, y, width - 2.0f * chamfer, chamfer, color);
    ui_fill(
        x + chamfer,
        y + height - chamfer,
        width - 2.0f * chamfer,
        chamfer,
        color
    );

    const float right = x + width;
    const float bottom = y + height;
    C2D_DrawTriangle(
        x, y + chamfer, color,
        x + chamfer, y + chamfer, color,
        x + chamfer, y, color,
        0.0f
    );
    C2D_DrawTriangle(
        right - chamfer, y, color,
        right - chamfer, y + chamfer, color,
        right, y + chamfer, color,
        0.0f
    );
    C2D_DrawTriangle(
        x, bottom - chamfer, color,
        x + chamfer, bottom - chamfer, color,
        x + chamfer, bottom, color,
        0.0f
    );
    C2D_DrawTriangle(
        right - chamfer, bottom, color,
        right - chamfer, bottom - chamfer, color,
        right, bottom - chamfer, color,
        0.0f
    );
}

static void ui_panel_outlined(
    float x,
    float y,
    float width,
    float height,
    float chamfer,
    u32 fill,
    u32 border
)
{
    ui_panel(x - 1.0f, y - 1.0f, width + 2.0f, height + 2.0f, chamfer + 1.0f, border);
    ui_panel(x, y, width, height, chamfer, fill);
}

static void ui_pill(float x, float y, float width, float height, u32 color)
{
    const float radius = height / 2.0f;
    if (width <= height) {
        C2D_DrawCircleSolid(x + radius, y + radius, 0.0f, radius, color);
        return;
    }
    ui_fill(x + radius, y, width - height, height, color);
    C2D_DrawCircleSolid(x + radius, y + radius, 0.0f, radius, color);
    C2D_DrawCircleSolid(x + width - radius, y + radius, 0.0f, radius, color);
}

static void ui_dot(float center_x, float center_y, float radius, u32 color)
{
    C2D_DrawCircleSolid(center_x, center_y, 0.0f, radius, color);
}

static void ui_spinner(
    float center_x,
    float center_y,
    float radius,
    float dot_radius,
    u32 color
)
{
    const int count = 8;
    const int head = (int)((frame_time_ms / 90u) % (u64)count);
    for (int index = 0; index < count; index++) {
        const float angle =
            (float)index * (2.0f * (float)M_PI / (float)count)
            - (float)M_PI / 2.0f;
        const int age = (head - index + count) % count;
        const u8 alpha = (u8)(35 + (210 * (count - 1 - age)) / (count - 1));
        ui_dot(
            center_x + cosf(angle) * radius,
            center_y + sinf(angle) * radius,
            dot_radius,
            ui_alpha(color, alpha)
        );
    }
}

/* Sweeping block used for work whose remaining duration is unknown. */
static void ui_indeterminate_bar(float x, float y, float width, u32 color)
{
    ui_fill(x, y, width, 3.0f, ui_alpha(UI_LINE, 220));

    const float phase = (float)(frame_time_ms % 1500u) / 1500.0f;
    const float block = width * 0.3f;
    const float start = x - block + phase * (width + block);
    float left = start;
    float right = start + block;
    if (left < x) {
        left = x;
    }
    if (right > x + width) {
        right = x + width;
    }
    if (right > left) {
        ui_fill_horizontal(
            left,
            y,
            right - left,
            3.0f,
            ui_alpha(color, 90),
            color
        );
    }
}

/* ------------------------------------------------------------------ text -- */

static float ui_codepoint_advance(u32 codepoint)
{
    if (codepoint == '\n' || codepoint == '\r') {
        return 0.0f;
    }
    if (codepoint >= 0x20 && codepoint < 0x80) {
        return ascii_advance[codepoint - 0x20];
    }

    const int glyph = C2D_FontGlyphIndexFromCodePoint(NULL, codepoint);
    const charWidthInfo_s *info = C2D_FontGetCharWidthInfo(NULL, glyph);
    return info != NULL ? (float)info->charWidth : fallback_advance;
}

static void ui_font_metrics_initialize(void)
{
    for (u32 codepoint = 0x20; codepoint < 0x80; codepoint++) {
        const int glyph = C2D_FontGlyphIndexFromCodePoint(NULL, codepoint);
        const charWidthInfo_s *info = C2D_FontGetCharWidthInfo(NULL, glyph);
        ascii_advance[codepoint - 0x20] =
            info != NULL ? (float)info->charWidth : 0.0f;
    }
    fallback_advance = ascii_advance['n' - 0x20];
}

static float ui_measure_bytes(const char *text, size_t length, float scale)
{
    float width = 0.0f;
    size_t index = 0;
    while (index < length && text[index] != '\0') {
        u32 codepoint = 0;
        const ssize_t used =
            decode_utf8(&codepoint, (const u8 *)(text + index));
        if (used <= 0) {
            break;
        }
        width += ui_codepoint_advance(codepoint);
        index += (size_t)used;
    }
    return width * scale;
}

static float ui_measure(const char *text, float scale)
{
    return text != NULL ? ui_measure_bytes(text, strlen(text), scale) : 0.0f;
}

static void ui_draw(
    float x,
    float y,
    float scale,
    u32 color,
    u32 align,
    const char *text
)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    C2D_Text parsed;
    if (C2D_TextParse(&parsed, text_buffer, text) == NULL) {
        return;
    }
    C2D_TextOptimize(&parsed);
    C2D_DrawText(
        &parsed,
        C2D_WithColor | align,
        x,
        y,
        0.0f,
        scale,
        scale,
        color
    );
}

static void ui_drawf(
    float x,
    float y,
    float scale,
    u32 color,
    u32 align,
    const char *format,
    ...
)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(scratch, sizeof(scratch), format, arguments);
    va_end(arguments);
    ui_draw(x, y, scale, color, align, scratch);
}

/* Draws `text`, replacing the overflowing tail with an ellipsis. */
static void ui_draw_clipped(
    float x,
    float y,
    float max_width,
    float scale,
    u32 color,
    u32 align,
    const char *text
)
{
    if (text == NULL || text[0] == '\0' || max_width <= 0.0f) {
        return;
    }
    if (ui_measure(text, scale) <= max_width) {
        ui_draw(x, y, scale, color, align, text);
        return;
    }

    const float ellipsis_width = ui_measure("...", scale);
    const float budget = max_width - ellipsis_width;
    float used = 0.0f;
    size_t index = 0;
    size_t kept = 0;
    while (text[index] != '\0' && kept + 4 < sizeof(scratch)) {
        u32 codepoint = 0;
        const ssize_t bytes =
            decode_utf8(&codepoint, (const u8 *)(text + index));
        if (bytes <= 0) {
            break;
        }
        const float advance = ui_codepoint_advance(codepoint) * scale;
        if (used + advance > budget) {
            break;
        }
        used += advance;
        memcpy(scratch + kept, text + index, (size_t)bytes);
        kept += (size_t)bytes;
        index += (size_t)bytes;
    }
    memcpy(scratch + kept, "...", 4);
    ui_draw(x, y, scale, color, align, scratch);
}

/* Copies one wrapped span into the scratch buffer, dropping carriage returns. */
static void ui_draw_span(
    float x,
    float y,
    float scale,
    u32 color,
    const char *source,
    const UiWrapLine *line
)
{
    size_t kept = 0;
    for (unsigned int index = 0; index < line->length; index++) {
        const char character = source[line->offset + index];
        if (character == '\r' || character == '\n') {
            continue;
        }
        if (kept + 1 >= sizeof(scratch)) {
            break;
        }
        scratch[kept++] = character;
    }
    scratch[kept] = '\0';
    ui_draw(x, y, scale, color, C2D_AlignLeft, scratch);
}

/*
 * Wrapping is recomputed only when the text actually changes. The callers hand
 * us reused fixed buffers, so identity and length are not enough to prove the
 * content is unchanged; hash it. A few kilobytes per frame is far cheaper than
 * re-wrapping, and it means no caller has to remember to invalidate anything.
 */
static u32 ui_hash(const char *text, size_t length)
{
    u32 hash = 2166136261u;
    for (size_t index = 0; index < length; index++) {
        hash ^= (u32)(unsigned char)text[index];
        hash *= 16777619u;
    }
    return hash;
}

static UiWrapCache *ui_wrap(
    int slot,
    const char *text,
    float width,
    float scale
)
{
    UiWrapCache *cache = &wrap_caches[slot];
    if (text == NULL) {
        text = "";
    }
    const size_t length = strlen(text);
    const u32 hash = ui_hash(text, length);
    if (cache->source == text
        && cache->source_length == length
        && cache->source_hash == hash
        && cache->width == width
        && cache->scale == scale) {
        return cache;
    }

    cache->source = text;
    cache->source_length = length;
    cache->source_hash = hash;
    cache->width = width;
    cache->scale = scale;
    cache->line_count = 0;

    size_t cursor = 0;
    while (cache->line_count < UI_WRAP_MAX_LINES && text[cursor] != '\0') {
        const size_t line_start = cursor;
        float used = 0.0f;
        size_t scan = cursor;
        size_t word_break = 0;
        bool has_word_break = false;
        bool newline = false;

        while (text[scan] != '\0') {
            if (text[scan] == '\n') {
                newline = true;
                break;
            }
            u32 codepoint = 0;
            const ssize_t bytes =
                decode_utf8(&codepoint, (const u8 *)(text + scan));
            if (bytes <= 0) {
                break;
            }
            const float advance = ui_codepoint_advance(codepoint) * scale;
            if (used + advance > width && scan > line_start) {
                break;
            }
            used += advance;
            scan += (size_t)bytes;
            if (codepoint == ' ') {
                word_break = scan;
                has_word_break = true;
            }
        }

        size_t line_end;
        if (newline) {
            line_end = scan;
            cursor = scan + 1;
        } else if (text[scan] == '\0') {
            line_end = scan;
            cursor = scan;
        } else if (has_word_break && word_break > line_start) {
            line_end = word_break;
            cursor = word_break;
            while (text[cursor] == ' ') {
                cursor++;
            }
        } else {
            line_end = scan;
            cursor = scan;
        }

        while (line_end > line_start && text[line_end - 1] == ' ') {
            line_end--;
        }

        cache->lines[cache->line_count].offset = (unsigned int)line_start;
        cache->lines[cache->line_count].length =
            (unsigned int)(line_end - line_start);
        cache->line_count++;
    }

    return cache;
}

/* ---------------------------------------------------------- state mapping -- */

typedef struct {
    const char *label;
    u32 color;
    bool busy;
} UiAgentBadge;

static UiAgentBadge ui_agent_badge(const UiModel *model)
{
    UiAgentBadge badge = { "UNKNOWN", UI_INK_FAINT, false };
    const char *state = model->agent_state != NULL ? model->agent_state : "";

    if (model->approval_pending || strcmp(state, "waiting_for_user") == 0) {
        badge.label = "APPROVAL";
        badge.color = UI_CORAL;
    } else if (strcmp(state, "working") == 0) {
        badge.label = "WORKING";
        badge.color = UI_AMBER;
        badge.busy = true;
    } else if (strcmp(state, "idle") == 0) {
        badge.label = "READY";
        badge.color = UI_MINT;
    } else if (strcmp(state, "connecting") == 0) {
        badge.label = "CONNECTING";
        badge.color = UI_AZURE;
        badge.busy = true;
    } else if (state[0] != '\0') {
        badge.label = state;
        badge.color = UI_INK_DIM;
    }
    return badge;
}

static u32 ui_link_color(const UiModel *model)
{
    const char *state = model->link_state != NULL ? model->link_state : "";
    if (!model->network_ready) {
        return UI_ROSE;
    }
    if (strcmp(state, "ready") == 0) {
        return UI_MINT;
    }
    /* Every phase network_push_state() reports as in-progress, not just two. */
    if (strcmp(state, "connecting") == 0
        || strcmp(state, "retrying") == 0
        || strcmp(state, "syncing") == 0) {
        return UI_AMBER;
    }
    return UI_INK_FAINT;
}

static void ui_format_duration(
    char *destination,
    size_t capacity,
    unsigned int milliseconds,
    bool hundredths
)
{
    if (hundredths) {
        snprintf(
            destination,
            capacity,
            "%u:%02u.%02u",
            milliseconds / 60000u,
            (milliseconds / 1000u) % 60u,
            (milliseconds % 1000u) / 10u
        );
    } else {
        snprintf(
            destination,
            capacity,
            "%u:%02u",
            milliseconds / 60000u,
            (milliseconds / 1000u) % 60u
        );
    }
}

/* --------------------------------------------------------- shared chrome -- */

static void ui_accent_rule(float x, float y, float width, float height)
{
    const float half = width / 2.0f;
    ui_fill_horizontal(x, y, half, height, UI_AZURE, UI_VIOLET);
    ui_fill_horizontal(x + half, y, half, height, UI_VIOLET, UI_MINT);
}

static void ui_badge(float right_x, float y, const UiAgentBadge *badge)
{
    const float height = 16.0f;
    const float text_width = ui_measure(badge->label, UI_SCALE_MICRO);
    const float width = text_width + 28.0f;
    const float x = right_x - width;

    ui_pill(x, y, width, height, ui_blend(badge->color, 40, UI_BG1));
    if (badge->busy) {
        ui_spinner(x + 10.0f, y + height / 2.0f, 4.0f, 1.4f, badge->color);
    } else {
        ui_dot(x + 10.0f, y + height / 2.0f, 3.0f, badge->color);
    }
    ui_draw(
        x + 19.0f,
        y + 2.0f,
        UI_SCALE_MICRO,
        badge->color,
        C2D_AlignLeft,
        badge->label
    );
}

static void ui_top_header(const UiModel *model, const char *subtitle)
{
    ui_fill(0.0f, 0.0f, UI_TOP_W, UI_HEADER_H, UI_BG1);
    ui_accent_rule(0.0f, 0.0f, UI_TOP_W, 2.0f);
    ui_fill(0.0f, UI_HEADER_H - 1.0f, UI_TOP_W, 1.0f, UI_LINE);

    ui_draw(UI_PAD, 6.0f, UI_SCALE_WORDMARK, UI_INK, C2D_AlignLeft, "3gent");
    const float wordmark_end =
        UI_PAD + ui_measure("3gent", UI_SCALE_WORDMARK);

    const UiAgentBadge badge = ui_agent_badge(model);
    ui_badge(UI_TOP_W - UI_PAD, 7.0f, &badge);
    const float badge_left =
        UI_TOP_W - UI_PAD - ui_measure(badge.label, UI_SCALE_MICRO) - 28.0f;

    ui_dot(wordmark_end + 7.0f, 15.0f, 1.5f, UI_INK_FAINT);
    ui_draw_clipped(
        wordmark_end + 14.0f,
        11.0f,
        badge_left - wordmark_end - 22.0f,
        UI_SCALE_LABEL,
        UI_INK_DIM,
        C2D_AlignLeft,
        subtitle
    );
}

/* ------------------------------------------------------- top: main screen -- */

static void ui_body_layout(
    const UiModel *model,
    float *text_top,
    size_t *visible_lines
)
{
    const bool has_prompt = model->prompt != NULL && model->prompt[0] != '\0';
    *text_top = UI_HEADER_H + 8.0f + (has_prompt ? 20.0f : 0.0f);
    const float available = UI_BODY_TEXT_BOTTOM - *text_top;
    *visible_lines = available > 0.0f
        ? (size_t)(available / UI_BODY_LINE_H)
        : 0;
}

static void ui_scrollbar(
    float x,
    float y,
    float height,
    size_t line_count,
    size_t visible_lines,
    size_t first_line
)
{
    if (line_count <= visible_lines || visible_lines == 0) {
        return;
    }
    ui_fill(x, y, 3.0f, height, ui_alpha(UI_LINE, 170));

    float thumb_height = height * (float)visible_lines / (float)line_count;
    if (thumb_height < 14.0f) {
        thumb_height = 14.0f;
    }
    const size_t max_first = line_count - visible_lines;
    const float progress = max_first > 0
        ? (float)first_line / (float)max_first
        : 0.0f;
    const float thumb_y = y + progress * (height - thumb_height);
    ui_fill(x, thumb_y, 3.0f, thumb_height, ui_alpha(UI_INK_DIM, 210));
}

/* While the microphone is open the read surface belongs to the microphone. */
static void ui_listening_body(const UiModel *model)
{
    const float center_x = UI_TOP_W / 2.0f;
    const float center_y = 106.0f;
    unsigned int percent = model->record_level_percent;
    if (percent > 100u) {
        percent = 100u;
    }
    const float level = (float)percent / 100.0f;

    ui_dot(center_x, center_y, 26.0f + level * 18.0f, ui_alpha(UI_ROSE, 26));
    ui_dot(center_x, center_y, 19.0f + level * 6.0f, ui_alpha(UI_ROSE, 52));
    ui_dot(center_x, center_y, 9.0f, UI_ROSE);

    ui_draw(
        center_x,
        150.0f,
        UI_SCALE_LABEL,
        UI_ROSE,
        C2D_AlignCenter,
        "LISTENING"
    );

    char elapsed[24];
    ui_format_duration(elapsed, sizeof(elapsed), model->record_ms, true);
    ui_draw(center_x, 170.0f, 0.86f, UI_INK, C2D_AlignCenter, elapsed);
    ui_draw(
        center_x,
        198.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignCenter,
        "release R to transcribe, or keep holding"
    );
}

static void ui_response_body(const UiModel *model)
{
    if (model->recording) {
        ui_listening_body(model);
        return;
    }

    float text_top = 0.0f;
    size_t visible_lines = 0;
    ui_body_layout(model, &text_top, &visible_lines);

    if (model->prompt != NULL && model->prompt[0] != '\0') {
        ui_fill(UI_PAD, UI_HEADER_H + 9.0f, 2.0f, 13.0f, UI_AZURE);
        ui_draw_clipped(
            UI_PAD + 8.0f,
            UI_HEADER_H + 7.0f,
            UI_TOP_W - 2.0f * UI_PAD - 8.0f,
            UI_SCALE_LABEL,
            UI_INK_DIM,
            C2D_AlignLeft,
            model->prompt
        );
    }

    const UiWrapCache *wrapped = ui_wrap(
        UI_WRAP_SLOT_RESPONSE,
        model->response,
        UI_BODY_W,
        UI_SCALE_BODY
    );

    if (wrapped->line_count == 0) {
        const float center_y = (text_top + UI_BODY_TEXT_BOTTOM) / 2.0f;
        ui_dot(UI_TOP_W / 2.0f, center_y - 14.0f, 9.0f, ui_alpha(UI_LINE, 255));
        ui_dot(UI_TOP_W / 2.0f, center_y - 14.0f, 6.0f, UI_BG0);
        ui_draw(
            UI_TOP_W / 2.0f,
            center_y,
            UI_SCALE_BODY,
            UI_INK_FAINT,
            C2D_AlignCenter,
            "Nothing from the agent yet"
        );
        ui_draw(
            UI_TOP_W / 2.0f,
            center_y + 16.0f,
            UI_SCALE_MICRO,
            ui_alpha(UI_INK_FAINT, 190),
            C2D_AlignCenter,
            model->microphone_ready
                ? "Hold R to speak, or press A to type"
                : "Press A to type"
        );
        return;
    }

    size_t latest_start = wrapped->line_count > visible_lines
        ? wrapped->line_count - visible_lines
        : 0;
    size_t scroll = model->scroll_lines;
    if (scroll > latest_start) {
        scroll = latest_start;
    }
    const size_t first_line = latest_start - scroll;
    size_t last_line = first_line + visible_lines;
    if (last_line > wrapped->line_count) {
        last_line = wrapped->line_count;
    }

    float y = text_top;
    for (size_t line = first_line; line < last_line; line++) {
        ui_draw_span(
            UI_PAD,
            y,
            UI_SCALE_BODY,
            UI_INK,
            model->response,
            &wrapped->lines[line]
        );
        y += UI_BODY_LINE_H;
    }

    ui_scrollbar(
        UI_TOP_W - UI_PAD + 5.0f,
        text_top,
        UI_BODY_TEXT_BOTTOM - text_top,
        wrapped->line_count,
        visible_lines,
        first_line
    );
}

/* The one thing the user must answer gets the whole read surface. */
static void ui_attention_card(
    const char *title,
    u32 accent,
    const char *body,
    const char *hint
)
{
    ui_fill(
        0.0f,
        UI_HEADER_H,
        UI_TOP_W,
        UI_FOOTER_Y - UI_HEADER_H,
        ui_alpha(UI_BG0, 236)
    );

    const float x = 22.0f;
    const float y = 48.0f;
    const float width = UI_TOP_W - 2.0f * x;
    const float height = 146.0f;

    ui_panel_outlined(
        x,
        y,
        width,
        height,
        UI_CHAMFER,
        UI_BG2,
        ui_alpha(accent, 120)
    );
    ui_fill(x, y + UI_CHAMFER, 3.0f, height - 2.0f * UI_CHAMFER, accent);

    ui_draw(x + 16.0f, y + 12.0f, UI_SCALE_LABEL, accent, C2D_AlignLeft, title);
    ui_fill(x + 16.0f, y + 31.0f, width - 32.0f, 1.0f, ui_alpha(UI_LINE, 220));

    const UiWrapCache *wrapped = ui_wrap(
        UI_WRAP_SLOT_CARD,
        body,
        width - 32.0f,
        UI_SCALE_BODY
    );
    const size_t limit = wrapped->line_count > 5 ? 5 : wrapped->line_count;
    float line_y = y + 40.0f;
    for (size_t line = 0; line < limit; line++) {
        ui_draw_span(
            x + 16.0f,
            line_y,
            UI_SCALE_BODY,
            UI_INK,
            body,
            &wrapped->lines[line]
        );
        line_y += UI_BODY_LINE_H;
    }
    if (wrapped->line_count > limit) {
        ui_drawf(
            x + 16.0f,
            line_y,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "+%u more line%s",
            (unsigned int)(wrapped->line_count - limit),
            wrapped->line_count - limit == 1 ? "" : "s"
        );
    }

    ui_draw(
        x + 16.0f,
        y + height - 20.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignLeft,
        hint
    );
}

static void ui_top_footer(const UiModel *model)
{
    ui_fill(0.0f, UI_FOOTER_Y, UI_TOP_W, UI_SCREEN_H - UI_FOOTER_Y, UI_BG1);
    ui_fill(0.0f, UI_FOOTER_Y, UI_TOP_W, 1.0f, UI_LINE);

    const float y = UI_FOOTER_Y + 6.0f;
    if (model->diff_known) {
        char token[32];
        float x = UI_PAD;

        snprintf(
            token,
            sizeof(token),
            "%u file%s",
            model->diff_files,
            model->diff_files == 1 ? "" : "s"
        );
        ui_draw(x, y, UI_SCALE_MICRO, UI_INK_DIM, C2D_AlignLeft, token);
        x += ui_measure(token, UI_SCALE_MICRO) + 10.0f;

        snprintf(token, sizeof(token), "+%u", model->diff_additions);
        ui_draw(x, y, UI_SCALE_MICRO, UI_MINT, C2D_AlignLeft, token);
        x += ui_measure(token, UI_SCALE_MICRO) + 8.0f;

        snprintf(token, sizeof(token), "-%u", model->diff_deletions);
        ui_draw(x, y, UI_SCALE_MICRO, UI_ROSE, C2D_AlignLeft, token);
    } else {
        ui_draw_clipped(
            UI_PAD,
            y,
            150.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            model->version != NULL ? model->version : ""
        );
    }

    const size_t max_scroll = ui_max_scroll(model);
    const size_t scroll = model->scroll_lines > max_scroll
        ? max_scroll
        : model->scroll_lines;
    if (scroll > 0) {
        ui_drawf(
            UI_TOP_W / 2.0f,
            y,
            UI_SCALE_MICRO,
            UI_AMBER,
            C2D_AlignCenter,
            "%u lines back",
            (unsigned int)scroll
        );
    } else if (model->photo_pending) {
        ui_draw(
            UI_TOP_W / 2.0f,
            y,
            UI_SCALE_MICRO,
            UI_AZURE,
            C2D_AlignCenter,
            "photo attached"
        );
    }

    ui_drawf(
        UI_TOP_W - UI_PAD,
        y,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignRight,
        "evt %u",
        model->event_cursor
    );
}

static void ui_render_top_main(const UiModel *model)
{
    ui_fill(0.0f, UI_HEADER_H, UI_TOP_W, UI_FOOTER_Y - UI_HEADER_H, UI_BG0);
    ui_top_header(
        model,
        model->session_label != NULL && model->session_label[0] != '\0'
            ? model->session_label
            : "no task selected"
    );
    ui_response_body(model);

    if (model->approval_pending) {
        ui_attention_card(
            "APPROVAL REQUIRED",
            UI_CORAL,
            model->approval_summary != NULL && model->approval_summary[0] != '\0'
                ? model->approval_summary
                : "The agent is asking for permission to continue.",
            "X  approve once        B  decline"
        );
    } else if (model->transcript_ready) {
        ui_attention_card(
            "REVIEW TRANSCRIPT",
            UI_MINT,
            model->transcript,
            "A  send        Y  edit        B  cancel"
        );
    }

    ui_top_footer(model);
}

/* ---------------------------------------------------- bottom: main screen -- */

#define UI_STATUS_H     52.0f
#define UI_HERO_Y       52.0f
#define UI_HERO_H       96.0f
#define UI_CHIPS_Y      148.0f
#define UI_BAR_Y        204.0f

static void ui_bottom_status(const UiModel *model, bool busy)
{
    ui_fill(0.0f, 0.0f, UI_BOT_W, UI_STATUS_H, UI_BG1);
    ui_fill(0.0f, UI_STATUS_H - 1.0f, UI_BOT_W, 1.0f, UI_LINE);

    ui_draw_clipped(
        14.0f,
        10.0f,
        UI_BOT_W - 52.0f,
        UI_SCALE_HEAD,
        UI_INK,
        C2D_AlignLeft,
        model->view_state != NULL ? model->view_state : ""
    );
    ui_draw_clipped(
        14.0f,
        28.0f,
        UI_BOT_W - 28.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignLeft,
        model->detail != NULL ? model->detail : ""
    );
    ui_draw_clipped(
        14.0f,
        39.0f,
        UI_BOT_W - 28.0f,
        UI_SCALE_MICRO,
        ui_alpha(UI_INK_FAINT, 220),
        C2D_AlignLeft,
        model->detail_secondary != NULL ? model->detail_secondary : ""
    );
    if (busy) {
        ui_spinner(UI_BOT_W - 20.0f, 18.0f, 5.0f, 1.7f, UI_AZURE);
    }
}

static void ui_microphone_glyph(float center_x, float center_y, u32 color)
{
    ui_dot(center_x, center_y, 18.0f, ui_alpha(color, 34));
    ui_fill(center_x - 4.5f, center_y - 10.0f, 9.0f, 10.0f, color);
    ui_dot(center_x, center_y - 10.0f, 4.5f, color);
    ui_dot(center_x, center_y, 4.5f, color);
    ui_fill(center_x - 8.0f, center_y + 5.0f, 16.0f, 2.0f, color);
    ui_fill(center_x - 1.0f, center_y + 7.0f, 2.0f, 4.0f, color);
    ui_fill(center_x - 6.0f, center_y + 11.0f, 12.0f, 2.0f, color);
}

static void ui_hero_push_to_talk(const UiModel *model)
{
    const float x = 12.0f;
    const float y = 60.0f;
    const float width = UI_BOT_W - 24.0f;
    const float height = 80.0f;
    const bool ready = model->microphone_ready;
    const u32 accent = ready ? UI_MINT : UI_INK_FAINT;

    ui_panel_outlined(
        x,
        y,
        width,
        height,
        UI_CHAMFER,
        UI_BG2,
        ui_alpha(accent, ready ? 90 : 50)
    );
    ui_microphone_glyph(x + 40.0f, y + height / 2.0f, accent);

    if (ready) {
        ui_draw(
            x + 78.0f,
            y + 20.0f,
            UI_SCALE_HEAD,
            UI_INK,
            C2D_AlignLeft,
            "HOLD  R  TO TALK"
        );
        ui_draw(
            x + 78.0f,
            y + 45.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            "release to transcribe and review"
        );
    } else {
        ui_draw(
            x + 78.0f,
            y + 20.0f,
            UI_SCALE_HEAD,
            UI_INK_DIM,
            C2D_AlignLeft,
            "MIC UNAVAILABLE"
        );
        ui_draw(
            x + 78.0f,
            y + 45.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "press A to type instead"
        );
    }
}

static void ui_hero_recording(const UiModel *model)
{
    const float x = 12.0f;
    const float y = 56.0f;
    const float width = UI_BOT_W - 24.0f;
    const float height = 88.0f;

    ui_panel_outlined(
        x,
        y,
        width,
        height,
        UI_CHAMFER,
        UI_BG2,
        ui_alpha(UI_ROSE, 140)
    );

    const float pulse =
        0.5f + 0.5f * sinf((float)(frame_time_ms % 1000u) / 1000.0f
            * 2.0f * (float)M_PI);
    ui_dot(
        x + 26.0f,
        y + 20.0f,
        11.0f,
        ui_alpha(UI_ROSE, (u8)(40.0f + 60.0f * pulse))
    );
    ui_dot(x + 26.0f, y + 20.0f, 6.0f, UI_ROSE);

    char elapsed[24];
    char limit[16];
    ui_format_duration(elapsed, sizeof(elapsed), model->record_ms, true);
    ui_format_duration(limit, sizeof(limit), model->record_max_ms, false);
    ui_draw(x + 46.0f, y + 7.0f, UI_SCALE_HEAD, UI_INK, C2D_AlignLeft, elapsed);
    ui_drawf(
        x + 46.0f + ui_measure(elapsed, UI_SCALE_HEAD) + 8.0f,
        y + 13.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignLeft,
        "/ %s",
        limit
    );
    ui_draw(
        x + width - 12.0f,
        y + 7.0f,
        UI_SCALE_MICRO,
        UI_ROSE,
        C2D_AlignRight,
        "REC"
    );

    /* Scrolling level history reads as motion even when the level is flat. */
    const float wave_left = x + 12.0f;
    const float wave_right = x + width - 12.0f;
    const float pitch = (wave_right - wave_left) / (float)UI_WAVE_BARS;
    const float bar_width = pitch - 3.0f;
    const float mid_y = y + 48.0f;
    for (unsigned int index = 0; index < UI_WAVE_BARS; index++) {
        const unsigned int slot = (wave_head + index) % UI_WAVE_BARS;
        const float level = (float)wave_levels[slot] / 100.0f;
        const float bar_height = 2.0f + level * 22.0f;
        const u8 alpha = (u8)(90.0f + level * 150.0f);
        ui_fill(
            wave_left + (float)index * pitch,
            mid_y - bar_height / 2.0f,
            bar_width,
            bar_height,
            ui_alpha(level > 0.55f ? UI_AMBER : UI_ROSE, alpha)
        );
    }

    const float track_w = wave_right - wave_left;
    const float fraction = model->record_max_ms > 0
        ? (float)model->record_ms / (float)model->record_max_ms
        : 0.0f;
    ui_fill(wave_left, y + 68.0f, track_w, 2.0f, ui_alpha(UI_LINE, 220));
    ui_fill(
        wave_left,
        y + 68.0f,
        track_w * (fraction > 1.0f ? 1.0f : fraction),
        2.0f,
        UI_ROSE
    );
    ui_draw(
        x + width / 2.0f,
        y + 74.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignCenter,
        "release R to send for transcription"
    );
}

static void ui_hero_working(void)
{
    const float x = 12.0f;
    const float y = 60.0f;
    const float width = UI_BOT_W - 24.0f;
    const float height = 80.0f;

    ui_panel_outlined(
        x,
        y,
        width,
        height,
        UI_CHAMFER,
        UI_BG2,
        ui_alpha(UI_AMBER, 90)
    );
    ui_spinner(x + 30.0f, y + 30.0f, 9.0f, 2.4f, UI_AMBER);
    ui_draw(
        x + 54.0f,
        y + 18.0f,
        UI_SCALE_HEAD,
        UI_INK,
        C2D_AlignLeft,
        "AGENT WORKING"
    );
    ui_draw(
        x + 54.0f,
        y + 41.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignLeft,
        "responses stream to the top screen"
    );
    ui_indeterminate_bar(x + 14.0f, y + height - 16.0f, width - 28.0f, UI_AMBER);
}

static void ui_hero_transcript(const UiModel *model)
{
    const float x = 12.0f;
    const float y = 60.0f;
    const float width = UI_BOT_W - 24.0f;
    const float height = 80.0f;

    ui_panel_outlined(
        x,
        y,
        width,
        height,
        UI_CHAMFER,
        UI_BG2,
        ui_alpha(UI_MINT, 110)
    );
    ui_draw(
        x + 16.0f,
        y + 12.0f,
        UI_SCALE_LABEL,
        UI_MINT,
        C2D_AlignLeft,
        "REVIEW IT ON THE TOP SCREEN"
    );
    ui_draw_clipped(
        x + 16.0f,
        y + 32.0f,
        width - 32.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignLeft,
        model->transcript != NULL ? model->transcript : ""
    );

    const float button_y = y + 50.0f;
    const float button_w = (width - 32.0f - 16.0f) / 3.0f;
    const char *labels[3] = { "A  send", "Y  edit", "B  cancel" };
    const u32 colors[3] = { UI_MINT, UI_AZURE, UI_INK_DIM };
    for (int index = 0; index < 3; index++) {
        const float button_x = x + 16.0f + (float)index * (button_w + 8.0f);
        ui_panel(
            button_x,
            button_y,
            button_w,
            18.0f,
            2.0f,
            ui_blend(colors[index], 44, UI_BG2)
        );
        ui_draw(
            button_x + button_w / 2.0f,
            button_y + 3.0f,
            UI_SCALE_MICRO,
            colors[index],
            C2D_AlignCenter,
            labels[index]
        );
    }
}

static void ui_hero_approval(const UiModel *model)
{
    const float x = 12.0f;
    const float y = 56.0f;
    const float width = UI_BOT_W - 24.0f;

    ui_draw_clipped(
        x,
        y,
        width,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignLeft,
        model->approval_summary != NULL && model->approval_summary[0] != '\0'
            ? model->approval_summary
            : "The agent needs permission."
    );

    const float button_y = y + 18.0f;
    const float button_h = 62.0f;
    const float button_w = (width - 10.0f) / 2.0f;

    ui_panel_outlined(
        x,
        button_y,
        button_w,
        button_h,
        UI_CHAMFER,
        ui_blend(UI_CORAL, 40, UI_BG0),
        ui_alpha(UI_CORAL, 170)
    );
    ui_draw(
        x + button_w / 2.0f,
        button_y + 14.0f,
        UI_SCALE_WORDMARK,
        UI_CORAL,
        C2D_AlignCenter,
        "X"
    );
    ui_draw(
        x + button_w / 2.0f,
        button_y + 40.0f,
        UI_SCALE_MICRO,
        UI_INK,
        C2D_AlignCenter,
        "APPROVE ONCE"
    );

    const float decline_x = x + button_w + 10.0f;
    ui_panel_outlined(
        decline_x,
        button_y,
        button_w,
        button_h,
        UI_CHAMFER,
        UI_BG2,
        ui_alpha(UI_LINE, 255)
    );
    ui_draw(
        decline_x + button_w / 2.0f,
        button_y + 14.0f,
        UI_SCALE_WORDMARK,
        UI_INK_DIM,
        C2D_AlignCenter,
        "B"
    );
    ui_draw(
        decline_x + button_w / 2.0f,
        button_y + 40.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignCenter,
        "DECLINE"
    );
}

typedef struct {
    const char *key;
    const char *label;
    bool enabled;
} UiChip;

static void ui_chip(float x, float y, float width, const UiChip *chip)
{
    const float cap_width = 22.0f;

    ui_panel_outlined(
        x,
        y,
        cap_width,
        18.0f,
        2.0f,
        chip->enabled ? UI_BG2 : ui_blend(UI_BG2, 130, UI_BG0),
        chip->enabled ? UI_LINE : ui_blend(UI_LINE, 110, UI_BG0)
    );
    ui_draw(
        x + cap_width / 2.0f,
        y + 2.0f,
        UI_SCALE_MICRO,
        chip->enabled ? UI_INK : ui_alpha(UI_INK_FAINT, 150),
        C2D_AlignCenter,
        chip->key
    );
    ui_draw_clipped(
        x + cap_width + 6.0f,
        y + 2.0f,
        width - cap_width - 6.0f,
        UI_SCALE_MICRO,
        chip->enabled ? UI_INK_DIM : ui_alpha(UI_INK_FAINT, 150),
        C2D_AlignLeft,
        chip->label
    );
}

static void ui_chip_grid(const UiChip *chips, size_t count)
{
    const float margin = 10.0f;
    const float gap = 6.0f;
    const float width = (UI_BOT_W - 2.0f * margin - 2.0f * gap) / 3.0f;
    for (size_t index = 0; index < count && index < 6; index++) {
        const float x = margin + (float)(index % 3) * (width + gap);
        const float y = UI_CHIPS_Y + 8.0f + (float)(index / 3) * 24.0f;
        ui_chip(x, y, width, &chips[index]);
    }
}

static void ui_bottom_bar(const UiModel *model)
{
    ui_fill(0.0f, UI_BAR_Y, UI_BOT_W, UI_SCREEN_H - UI_BAR_Y, UI_BG1);
    ui_fill(0.0f, UI_BAR_Y, UI_BOT_W, 1.0f, UI_LINE);

    const u32 link_color = ui_link_color(model);
    ui_dot(14.0f, UI_BAR_Y + 12.0f, 3.0f, link_color);
    ui_draw_clipped(
        23.0f,
        UI_BAR_Y + 6.0f,
        120.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignLeft,
        model->network_ready
            ? (model->link_state != NULL ? model->link_state : "unknown")
            : "no network"
    );
    ui_drawf(
        UI_BOT_W - 12.0f,
        UI_BAR_Y + 6.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignRight,
        "%s:%u",
        model->server_host != NULL ? model->server_host : "?",
        model->server_port
    );

    const float token_y = UI_BAR_Y + 21.0f;
    float token_x = 14.0f;
    const char *tokens[3] = { "mic", "audio link", "photo" };
    const bool states[3] = {
        model->microphone_ready,
        model->audio_warm,
        model->photo_pending,
    };
    const u32 colors[3] = { UI_MINT, UI_MINT, UI_AZURE };
    for (int index = 0; index < 3; index++) {
        const u32 color = states[index] ? colors[index] : UI_INK_FAINT;
        ui_dot(token_x + 2.0f, token_y + 5.0f, 2.0f, color);
        ui_draw(
            token_x + 8.0f,
            token_y,
            UI_SCALE_MICRO,
            ui_alpha(color, states[index] ? 230 : 150),
            C2D_AlignLeft,
            tokens[index]
        );
        token_x += ui_measure(tokens[index], UI_SCALE_MICRO) + 22.0f;
    }
}

static void ui_render_bottom_main(const UiModel *model)
{
    ui_fill(0.0f, UI_HERO_Y, UI_BOT_W, UI_BAR_Y - UI_HERO_Y, UI_BG0);
    /* A pending approval is waiting on the user, not on progress. */
    ui_bottom_status(
        model,
        model->recording || (model->turn_active && !model->approval_pending)
    );

    if (model->recording) {
        ui_hero_recording(model);
    } else if (model->approval_pending) {
        ui_hero_approval(model);
    } else if (model->transcript_ready) {
        ui_hero_transcript(model);
    } else if (model->turn_active) {
        ui_hero_working();
    } else {
        ui_hero_push_to_talk(model);
    }

    const bool free_to_act =
        !model->recording && !model->turn_active && !model->transcript_ready;
    const UiChip chips[6] = {
        {
            "A",
            model->transcript_ready ? "Send" : "Type",
            model->transcript_ready || (free_to_act && !model->approval_pending),
        },
        {
            "X",
            model->approval_pending ? "Approve" : "Ask demo",
            model->approval_pending || free_to_act,
        },
        {
            "B",
            model->transcript_ready
                ? "Cancel"
                : (model->approval_pending ? "Decline" : "Interrupt"),
            model->transcript_ready || model->approval_pending
                || model->turn_active,
        },
        { "L", "Photo", free_to_act && !model->approval_pending },
        { "UD", "Scroll", true },
        { "ST", "Exit", true },
    };
    ui_chip_grid(chips, 6);
    ui_bottom_bar(model);
}

/* ------------------------------------------------------------- boot view -- */

static void ui_render_top_boot(const UiModel *model)
{
    ui_fill_vertical(0.0f, 0.0f, UI_TOP_W, UI_SCREEN_H, UI_BG1, UI_BG0);

    const float center_x = UI_TOP_W / 2.0f;
    ui_draw(center_x, 88.0f, 1.15f, UI_INK, C2D_AlignCenter, "3gent");
    ui_accent_rule(center_x - 54.0f, 126.0f, 108.0f, 2.0f);
    ui_draw(
        center_x,
        136.0f,
        UI_SCALE_LABEL,
        UI_INK_DIM,
        C2D_AlignCenter,
        "a pocket remote for coding agents"
    );
    ui_draw_clipped(
        center_x,
        216.0f,
        360.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignCenter,
        model->version != NULL ? model->version : ""
    );
}

static void ui_render_bottom_boot(const UiModel *model)
{
    ui_fill(0.0f, 0.0f, UI_BOT_W, UI_SCREEN_H, UI_BG0);
    ui_spinner(UI_BOT_W / 2.0f, 92.0f, 12.0f, 3.0f, UI_AZURE);
    ui_draw_clipped(
        UI_BOT_W / 2.0f,
        118.0f,
        UI_BOT_W - 24.0f,
        UI_SCALE_HEAD,
        UI_INK,
        C2D_AlignCenter,
        model->view_state != NULL ? model->view_state : ""
    );
    ui_draw_clipped(
        UI_BOT_W / 2.0f,
        142.0f,
        UI_BOT_W - 24.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignCenter,
        model->detail != NULL ? model->detail : ""
    );
    ui_bottom_bar(model);
}

/* ---------------------------------------------------------- task chooser -- */

static void ui_render_top_sessions(const UiModel *model)
{
    ui_fill(0.0f, UI_HEADER_H, UI_TOP_W, UI_SCREEN_H - UI_HEADER_H, UI_BG0);
    ui_top_header(model, "choose a coding-agent task");

    ui_draw(
        UI_PAD,
        UI_HEADER_H + 8.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignLeft,
        "RECENT TASKS"
    );

    if (model->session_count == 0) {
        const char *headline = "No recent tasks were returned";
        const char *hint = "X starts a new task in the bridge workspace";
        if (model->sessions_loading) {
            headline = "Looking for recent tasks...";
            hint = "";
        } else if (model->sessions_retryable) {
            headline = "The bridge did not answer";
            hint = "Check that it is running, then retry with A or X";
        }
        if (model->sessions_loading) {
            ui_spinner(UI_TOP_W / 2.0f, 96.0f, 9.0f, 2.4f, UI_AZURE);
        }
        ui_draw(
            UI_TOP_W / 2.0f,
            120.0f,
            UI_SCALE_BODY,
            model->sessions_retryable ? UI_ROSE : UI_INK_FAINT,
            C2D_AlignCenter,
            headline
        );
        ui_draw(
            UI_TOP_W / 2.0f,
            142.0f,
            UI_SCALE_MICRO,
            ui_alpha(UI_INK_FAINT, 190),
            C2D_AlignCenter,
            hint
        );
        return;
    }

    const float row_height = 26.0f;
    float y = UI_HEADER_H + 26.0f;
    for (size_t index = 0; index < model->session_count; index++) {
        const bool selected = index == model->session_selected;
        if (selected) {
            ui_panel(
                UI_PAD - 4.0f,
                y,
                UI_TOP_W - 2.0f * UI_PAD + 8.0f,
                row_height - 3.0f,
                2.0f,
                UI_BG2
            );
            ui_fill(UI_PAD - 4.0f, y, 3.0f, row_height - 3.0f, UI_AZURE);
        }
        ui_drawf(
            UI_PAD + 8.0f,
            y + 4.0f,
            UI_SCALE_MICRO,
            selected ? UI_AZURE : UI_INK_FAINT,
            C2D_AlignLeft,
            "%u",
            (unsigned int)(index + 1)
        );
        ui_draw_clipped(
            UI_PAD + 24.0f,
            y + 3.0f,
            UI_TOP_W - 2.0f * UI_PAD - 28.0f,
            UI_SCALE_BODY,
            selected ? UI_INK : UI_INK_DIM,
            C2D_AlignLeft,
            model->session_labels[index]
        );
        y += row_height;
    }
}

static void ui_render_bottom_sessions(const UiModel *model)
{
    ui_fill(0.0f, UI_HERO_Y, UI_BOT_W, UI_BAR_Y - UI_HERO_Y, UI_BG0);
    ui_bottom_status(model, model->sessions_loading);

    const float x = 12.0f;
    const float y = 62.0f;
    const float width = UI_BOT_W - 24.0f;
    const float button_h = 34.0f;

    if (model->sessions_retryable) {
        ui_panel_outlined(
            x,
            y,
            width,
            button_h + 8.0f + button_h,
            UI_CHAMFER,
            UI_BG2,
            ui_alpha(UI_ROSE, 110)
        );
        ui_draw(
            x + 14.0f,
            y + 12.0f,
            UI_SCALE_LABEL,
            UI_ROSE,
            C2D_AlignLeft,
            "Could not reach the bridge"
        );
        ui_draw(
            x + 14.0f,
            y + 38.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            "A or X retries task discovery"
        );
    } else {
        const bool can_resume = model->session_count > 0;
        ui_panel_outlined(
            x,
            y,
            width,
            button_h,
            UI_CHAMFER,
            can_resume ? ui_blend(UI_AZURE, 34, UI_BG0) : UI_BG2,
            can_resume ? ui_alpha(UI_AZURE, 150) : UI_LINE
        );
        ui_draw(
            x + 14.0f,
            y + 8.0f,
            UI_SCALE_LABEL,
            can_resume ? UI_INK : UI_INK_FAINT,
            C2D_AlignLeft,
            "A   Resume selected task"
        );

        ui_panel_outlined(
            x,
            y + button_h + 8.0f,
            width,
            button_h,
            UI_CHAMFER,
            UI_BG2,
            ui_alpha(UI_LINE, 255)
        );
        ui_draw(
            x + 14.0f,
            y + button_h + 16.0f,
            UI_SCALE_LABEL,
            UI_INK,
            C2D_AlignLeft,
            "X   Start a new task"
        );
    }

    const UiChip chips[3] = {
        { "UD", "Choose", !model->sessions_retryable && model->session_count > 1 },
        { "A", model->sessions_retryable ? "Retry" : "Resume", true },
        { "ST", "Exit", true },
    };
    ui_chip_grid(chips, 3);

    ui_draw_clipped(
        12.0f,
        UI_CHIPS_Y + 34.0f,
        UI_BOT_W - 24.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignLeft,
        model->sessions_status != NULL ? model->sessions_status : ""
    );
    ui_bottom_bar(model);
}

/* --------------------------------------------------------- photo preview -- */

static void ui_render_top_photo(const UiModel *model)
{
    ui_fill(0.0f, 0.0f, UI_TOP_W, UI_SCREEN_H, UI_BG0);
    if (photo_texture_ready) {
        C2D_DrawImageAt(photo_image, 0.0f, 0.0f, 0.0f, NULL, 1.0f, 1.0f);
    } else {
        ui_draw(
            UI_TOP_W / 2.0f,
            110.0f,
            UI_SCALE_BODY,
            UI_INK_FAINT,
            C2D_AlignCenter,
            "No preview available"
        );
    }

    ui_fill_vertical(
        0.0f,
        UI_SCREEN_H - 34.0f,
        UI_TOP_W,
        34.0f,
        ui_alpha(UI_BG0, 0),
        ui_alpha(UI_BG0, 235)
    );
    ui_draw_clipped(
        UI_PAD,
        UI_SCREEN_H - 20.0f,
        UI_TOP_W - 2.0f * UI_PAD,
        UI_SCALE_MICRO,
        UI_INK,
        C2D_AlignLeft,
        model->photo_caption != NULL ? model->photo_caption : ""
    );
}

static void ui_render_bottom_photo(const UiModel *model)
{
    ui_fill(0.0f, UI_HERO_Y, UI_BOT_W, UI_BAR_Y - UI_HERO_Y, UI_BG0);
    ui_bottom_status(
        model,
        model->photo_progress_percent != UI_PHOTO_PROGRESS_NONE
    );

    const float x = 12.0f;
    const float y = 62.0f;
    const float width = UI_BOT_W - 24.0f;

    if (model->photo_progress_percent != UI_PHOTO_PROGRESS_NONE) {
        ui_panel_outlined(
            x,
            y,
            width,
            60.0f,
            UI_CHAMFER,
            UI_BG2,
            ui_alpha(UI_AZURE, 90)
        );
        ui_draw(
            x + 16.0f,
            y + 12.0f,
            UI_SCALE_LABEL,
            UI_INK,
            C2D_AlignLeft,
            "Uploading photo"
        );
        ui_drawf(
            x + width - 16.0f,
            y + 12.0f,
            UI_SCALE_LABEL,
            UI_AZURE,
            C2D_AlignRight,
            "%u%%",
            model->photo_progress_percent
        );
        const float track_x = x + 16.0f;
        const float track_w = width - 32.0f;
        ui_fill(track_x, y + 38.0f, track_w, 3.0f, ui_alpha(UI_LINE, 220));
        ui_fill(
            track_x,
            y + 38.0f,
            track_w * (float)model->photo_progress_percent / 100.0f,
            3.0f,
            UI_AZURE
        );
    } else {
        const float button_w = (width - 10.0f) / 2.0f;
        ui_panel_outlined(
            x,
            y,
            button_w,
            60.0f,
            UI_CHAMFER,
            ui_blend(UI_MINT, 36, UI_BG0),
            ui_alpha(UI_MINT, 155)
        );
        ui_draw(
            x + button_w / 2.0f,
            y + 12.0f,
            UI_SCALE_WORDMARK,
            UI_MINT,
            C2D_AlignCenter,
            "A"
        );
        ui_draw(
            x + button_w / 2.0f,
            y + 38.0f,
            UI_SCALE_MICRO,
            UI_INK,
            C2D_AlignCenter,
            "ATTACH"
        );

        const float cancel_x = x + button_w + 10.0f;
        ui_panel_outlined(
            cancel_x,
            y,
            button_w,
            60.0f,
            UI_CHAMFER,
            UI_BG2,
            ui_alpha(UI_LINE, 255)
        );
        ui_draw(
            cancel_x + button_w / 2.0f,
            y + 12.0f,
            UI_SCALE_WORDMARK,
            UI_INK_DIM,
            C2D_AlignCenter,
            "B"
        );
        ui_draw(
            cancel_x + button_w / 2.0f,
            y + 38.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignCenter,
            "DISCARD"
        );
    }

    ui_draw(
        UI_BOT_W / 2.0f,
        UI_CHIPS_Y + 12.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignCenter,
        "an attached photo is consumed by the next prompt"
    );
    ui_bottom_bar(model);
}

/* -------------------------------------------------------- photo texture -- */

static inline u32 ui_morton_offset(u32 x, u32 y)
{
    x = (x | (x << 2)) & 0x33u;
    x = (x | (x << 1)) & 0x55u;
    y = (y | (y << 2)) & 0x33u;
    y = (y | (y << 1)) & 0x55u;
    return x | (y << 1);
}

static inline size_t ui_tiled_offset(
    unsigned int x,
    unsigned int y,
    unsigned int texture_width
)
{
    const unsigned int tiles_per_row = texture_width >> 3;
    return ((size_t)(y >> 3) * tiles_per_row + (x >> 3)) * 64u
        + ui_morton_offset(x & 7u, y & 7u);
}

/*
 * Whether sampled v runs bottom-up over stored texture rows. Glyph 0 always
 * occupies the top-left cell of the first system-font sheet, so its reported
 * top texture coordinate identifies the convention on this libctru build
 * instead of us hard-coding a guess.
 */
static bool ui_detect_texture_v_flip(void)
{
    fontGlyphPos_s position;
    memset(&position, 0, sizeof(position));
    C2D_FontCalcGlyphPos(NULL, &position, 0, 0, 1.0f, 1.0f);
    return position.texcoord.top > 0.5f;
}

#define UI_PHOTO_TEX_W 512u
#define UI_PHOTO_TEX_H 256u

bool ui_photo_preview_set(
    const u8 *rgb565,
    unsigned int width,
    unsigned int height
)
{
    ui_photo_preview_clear();
    if (rgb565 == NULL
        || width == 0
        || height == 0
        || width > UI_PHOTO_TEX_W
        || height > UI_PHOTO_TEX_H) {
        return false;
    }
    if (!C3D_TexInit(
            &photo_texture,
            (u16)UI_PHOTO_TEX_W,
            (u16)UI_PHOTO_TEX_H,
            GPU_RGB565
        )) {
        return false;
    }
    C3D_TexSetFilter(&photo_texture, GPU_LINEAR, GPU_NEAREST);

    u16 *destination = (u16 *)photo_texture.data;
    const u16 *source = (const u16 *)(const void *)rgb565;
    memset(destination, 0, (size_t)UI_PHOTO_TEX_W * UI_PHOTO_TEX_H * 2u);
    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 0; x < width; x++) {
            destination[ui_tiled_offset(x, y, UI_PHOTO_TEX_W)] =
                source[(size_t)y * width + x];
        }
    }
    C3D_TexFlush(&photo_texture);

    photo_subtexture.width = (u16)width;
    photo_subtexture.height = (u16)height;
    photo_subtexture.left = 0.0f;
    photo_subtexture.right = (float)width / (float)UI_PHOTO_TEX_W;
    if (texture_v_axis_flipped) {
        photo_subtexture.top = 1.0f;
        photo_subtexture.bottom = 1.0f - (float)height / (float)UI_PHOTO_TEX_H;
    } else {
        photo_subtexture.top = 0.0f;
        photo_subtexture.bottom = (float)height / (float)UI_PHOTO_TEX_H;
    }
    photo_image.tex = &photo_texture;
    photo_image.subtex = &photo_subtexture;
    photo_texture_ready = true;
    return true;
}

void ui_photo_preview_clear(void)
{
    if (photo_texture_ready) {
        C3D_TexDelete(&photo_texture);
        photo_texture_ready = false;
    }
}

/* ------------------------------------------------------------ public API -- */

bool ui_initialize(char *error, size_t error_capacity)
{
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        snprintf(error, error_capacity, "citro3d initialization failed");
        gfxExit();
        return false;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        snprintf(error, error_capacity, "citro2d initialization failed");
        C3D_Fini();
        gfxExit();
        return false;
    }
    C2D_Prepare();

    top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    text_buffer = C2D_TextBufNew(UI_TEXT_GLYPH_CAPACITY);
    if (top_target == NULL || bottom_target == NULL || text_buffer == NULL) {
        snprintf(error, error_capacity, "citro2d render targets unavailable");
        ui_shutdown();
        return false;
    }

    ui_font_metrics_initialize();
    texture_v_axis_flipped = ui_detect_texture_v_flip();
    return true;
}

void ui_shutdown(void)
{
    ui_photo_preview_clear();
    if (text_buffer != NULL) {
        C2D_TextBufDelete(text_buffer);
        text_buffer = NULL;
    }
    top_target = NULL;
    bottom_target = NULL;
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

size_t ui_max_scroll(const UiModel *model)
{
    float text_top = 0.0f;
    size_t visible_lines = 0;
    ui_body_layout(model, &text_top, &visible_lines);

    const UiWrapCache *wrapped = ui_wrap(
        UI_WRAP_SLOT_RESPONSE,
        model->response,
        UI_BODY_W,
        UI_SCALE_BODY
    );
    if (wrapped->line_count <= visible_lines) {
        return 0;
    }
    return wrapped->line_count - visible_lines;
}

static void ui_update_wave(const UiModel *model)
{
    if (!model->recording) {
        wave_was_recording = false;
        return;
    }
    if (!wave_was_recording) {
        memset(wave_levels, 0, sizeof(wave_levels));
        wave_head = 0;
        wave_tick = 0;
        wave_was_recording = true;
    }
    if (++wave_tick % 2u != 0u) {
        return;
    }
    unsigned int level = model->record_level_percent;
    if (level > 100u) {
        level = 100u;
    }
    wave_levels[wave_head] = (u8)level;
    wave_head = (wave_head + 1u) % UI_WAVE_BARS;
}

void ui_render(const UiModel *model)
{
    frame_time_ms = osGetTime();
    ui_update_wave(model);
    C2D_TextBufClear(text_buffer);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    C2D_TargetClear(top_target, UI_BG0);
    C2D_SceneBegin(top_target);
    switch (model->screen) {
        case UI_SCREEN_BOOT:
            ui_render_top_boot(model);
            break;
        case UI_SCREEN_SESSIONS:
            ui_render_top_sessions(model);
            break;
        case UI_SCREEN_PHOTO:
            ui_render_top_photo(model);
            break;
        case UI_SCREEN_MAIN:
        default:
            ui_render_top_main(model);
            break;
    }

    C2D_TargetClear(bottom_target, UI_BG0);
    C2D_SceneBegin(bottom_target);
    switch (model->screen) {
        case UI_SCREEN_BOOT:
            ui_render_bottom_boot(model);
            break;
        case UI_SCREEN_SESSIONS:
            ui_render_bottom_sessions(model);
            break;
        case UI_SCREEN_PHOTO:
            ui_render_bottom_photo(model);
            break;
        case UI_SCREEN_MAIN:
        default:
            ui_render_bottom_main(model);
            break;
    }

    C3D_FrameEnd(0);
}
