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

/* Diameter of a round key cap, and the height a shoulder cap is built from. */
#define UI_KEY_CAP          18.0f

/* ------------------------------------------------------------- internals -- */

#define UI_TEXT_GLYPH_CAPACITY 3072
#define UI_SCRATCH_CAPACITY    512
#define UI_WRAP_SLOT_RESPONSE  0
#define UI_WRAP_SLOT_CARD      1
/*
 * The bottom screen wraps the same approval text as the top card but to a
 * different width, so it gets its own slot rather than thrashing the card's.
 */
#define UI_WRAP_SLOT_HERO      2
#define UI_WRAP_SLOTS          3
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

/*
 * The large panels: the approval card, the paired-machine card, the task detail
 * card, the decision hero.
 *
 * The accent used to be a three-pixel bar sitting on the panel's own left edge.
 * On hardware that reads as a stroke floating beside the panel rather than part
 * of it: the border it sits against is one dim pixel, and the chamfer cuts the
 * bar short at both ends so it never meets a corner. The accent is now a tick
 * inset inside the surface — the same grammar the bottom-screen list rows use,
 * which is the one that already reads well on the panel — and the fill and
 * border are mixed toward the accent so the whole box carries the state instead
 * of one edge of it.
 */
static void ui_card(
    float x,
    float y,
    float width,
    float height,
    u32 accent
)
{
    ui_panel_outlined(
        x,
        y,
        width,
        height,
        UI_CHAMFER,
        ui_blend(accent, 16, UI_BG2),
        ui_blend(accent, 150, UI_LINE)
    );
    ui_fill(x + 5.0f, y + 7.0f, 2.0f, height - 14.0f, accent);
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

    /*
     * The task manager is not inside a task, so the per-task agent state is
     * stale there and says "CONNECTING" for as long as the user stays on the
     * list. What the screen can actually answer is whether the bridge is
     * talking to us — and a list of its tasks is the proof that it is.
     */
    if (model->screen == UI_SCREEN_SESSIONS) {
        if (model->tasks_loading) {
            badge.label = "LOADING";
            badge.color = UI_AZURE;
            badge.busy = true;
        } else if (model->tasks_retryable) {
            badge.label = "OFFLINE";
            badge.color = UI_ROSE;
        } else {
            badge.label = "CONNECTED";
            badge.color = UI_MINT;
        }
        return badge;
    }

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

static u32 ui_task_accent(UiTaskState state)
{
    switch (state) {
        case UI_TASK_WORKING:   return UI_AMBER;
        case UI_TASK_ATTENTION: return UI_CORAL;
        case UI_TASK_FAILED:    return UI_ROSE;
        case UI_TASK_IDLE:      return UI_MINT;
        case UI_TASK_UNKNOWN:
        default:                return UI_INK_FAINT;
    }
}

static const char *ui_task_state_word(UiTaskState state)
{
    switch (state) {
        case UI_TASK_WORKING:   return "WORKING";
        case UI_TASK_ATTENTION: return "NEEDS YOU";
        case UI_TASK_FAILED:    return "FAILED";
        case UI_TASK_IDLE:      return "READY";
        case UI_TASK_UNKNOWN:
        default:                return "UNKNOWN";
    }
}

/*
 * Tasks that want the user, excluding the one already on screen. This is the
 * number that decides whether it is worth leaving the current task, so it is
 * shown wherever the user might be about to stop paying attention.
 */
static unsigned int ui_attention_elsewhere(const UiModel *model)
{
    unsigned int count = 0;
    if (model->tasks == NULL) {
        return 0;
    }
    for (size_t index = 0; index < model->task_count; index++) {
        if (model->task_active_valid && index == model->task_active) {
            continue;
        }
        if (model->tasks[index].state == UI_TASK_ATTENTION
            || model->tasks[index].unread) {
            count++;
        }
    }
    return count;
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

/*
 * A key cap is drawn as the key is actually shaped: A, B, X, Y, START and
 * SELECT are round on the hardware, and only the shoulders are rectangles. The
 * cap is the button, so the two have to agree — a square A teaches the thumb to
 * look in the wrong place.
 */
static bool ui_key_is_shoulder(const char *cap)
{
    if (cap == NULL) {
        return false;
    }
    const char *letters = cap[0] == 'Z' ? cap + 1 : cap;
    return (letters[0] == 'L' || letters[0] == 'R') && letters[1] == '\0';
}

/* A shoulder cap is wider than tall, which is the shape under the finger. */
static float ui_key_cap_width(float size, const char *cap)
{
    return ui_key_is_shoulder(cap) ? size + 4.0f : size;
}

static void ui_key_cap(
    float center_x,
    float center_y,
    float size,
    const char *cap,
    u32 fill,
    u32 ink
)
{
    const float width = ui_key_cap_width(size, cap);
    if (ui_key_is_shoulder(cap)) {
        const float height = size * 0.72f;
        ui_panel(
            center_x - width / 2.0f,
            center_y - height / 2.0f,
            width,
            height,
            2.0f,
            fill
        );
    } else {
        ui_dot(center_x, center_y, size / 2.0f, fill);
    }
    /*
     * The font box has more room above the glyph than below it, so centring the
     * box would sit the letter low. This offset centres the letter itself, and
     * is tuned for UI_SCALE_MICRO — the only scale a cap is ever drawn at.
     */
    ui_draw(center_x, center_y - 7.0f, UI_SCALE_MICRO, ink, C2D_AlignCenter, cap);
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
    float cursor = UI_PAD + ui_measure("3gent", UI_SCALE_WORDMARK);

    const UiAgentBadge badge = ui_agent_badge(model);
    ui_badge(UI_TOP_W - UI_PAD, 7.0f, &badge);
    const float badge_left =
        UI_TOP_W - UI_PAD - ui_measure(badge.label, UI_SCALE_MICRO) - 28.0f;

    ui_dot(cursor + 7.0f, 15.0f, 1.5f, UI_INK_FAINT);
    cursor += 14.0f;

    /*
     * Which of how many. Without it, switching tasks changes the label and the
     * user has no way to tell whether they moved one step or wrapped around.
     * The manager states the same thing on its card, so it does not repeat it.
     */
    if (model->screen == UI_SCREEN_MAIN
        && model->task_active_valid
        && model->task_count > 1) {
        char position[16];
        snprintf(
            position,
            sizeof(position),
            "%u/%u",
            (unsigned int)(model->task_active + 1),
            (unsigned int)model->task_count
        );
        const float chip_width = ui_measure(position, UI_SCALE_MICRO) + 12.0f;
        ui_panel(cursor, 9.0f, chip_width, 13.0f, 2.0f, UI_BG2);
        ui_draw(
            cursor + chip_width / 2.0f,
            10.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignCenter,
            position
        );
        cursor += chip_width + 6.0f;
    }

    ui_draw_clipped(
        cursor,
        11.0f,
        badge_left - cursor - 8.0f,
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
        "release to transcribe, or keep holding"
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

    ui_card(x, y, width, height, accent);

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

    /*
     * The right slot belongs to whatever most deserves the user's next glance:
     * another task waiting on them beats the event cursor, which is diagnostic.
     */
    const unsigned int waiting = ui_attention_elsewhere(model);
    if (waiting > 0) {
        ui_drawf(
            UI_TOP_W - UI_PAD,
            y,
            UI_SCALE_MICRO,
            UI_CORAL,
            C2D_AlignRight,
            "%u other task%s need%s you",
            waiting,
            waiting == 1 ? "" : "s",
            waiting == 1 ? "s" : ""
        );
    } else {
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
            "A  approve once        B  decline"
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

/*
 * The bottom screen is the action surface and the only touchable one, so its
 * bands are fixed and shared by every screen. `ui_hit_test` walks the same
 * geometry the renderer draws, which is what keeps a target underneath the
 * thing that looks like it.
 *
 *   0   task rail        (main screen only)
 *   32  status band      what is happening, plus the scroll cluster
 *   76  content          hero, list, or decision
 *   152 action bar       only the actions that are live right now
 *   204 status bar       link, endpoint, capabilities
 */
#define UI_RAIL_H       32.0f
#define UI_RAIL_TABS    4u
#define UI_RAIL_MORE_W  42.0f
#define UI_STATUS_H     46.0f
#define UI_CONTENT_Y    (UI_RAIL_H + UI_STATUS_H)
#define UI_ACTIONS_Y    152.0f
#define UI_ACTIONS_H    52.0f
#define UI_BAR_Y        204.0f

#define UI_ACTIONS_MAX  4u
#define UI_ACTION_MARGIN 10.0f
#define UI_ACTION_GAP    6.0f

/* One live action: a button on screen, a physical key, and the same handler. */
typedef struct {
    UiHitKind hit;
    const char *cap;
    const char *label;
    u32 accent;
} UiAction;

/* ------------------------------------------------------- scroll cluster -- */

#define UI_SCROLL_BUTTON   26.0f
#define UI_SCROLL_GAP      4.0f
#define UI_SCROLL_CLUSTER_W \
    (3.0f * UI_SCROLL_BUTTON + 2.0f * UI_SCROLL_GAP)

static float ui_scroll_cluster_x(void)
{
    return UI_BOT_W - 10.0f - UI_SCROLL_CLUSTER_W;
}

static void ui_triangle_glyph(float center_x, float center_y, float size, int direction, u32 color)
{
    const float half = size / 2.0f;
    if (direction < 0) {
        C2D_DrawTriangle(
            center_x, center_y - half, color,
            center_x - half, center_y + half, color,
            center_x + half, center_y + half, color,
            0.0f
        );
    } else {
        C2D_DrawTriangle(
            center_x, center_y + half, color,
            center_x - half, center_y - half, color,
            center_x + half, center_y - half, color,
            0.0f
        );
    }
}

/*
 * Reading a long answer is the most common thing to do on this screen and the
 * top screen cannot be touched, so scrolling gets its own targets. It appears
 * only when there is something above the fold.
 */
static void ui_scroll_cluster(const UiModel *model, float band_y)
{
    const float x = ui_scroll_cluster_x();
    const float y = band_y + (UI_STATUS_H - UI_SCROLL_BUTTON) / 2.0f;
    const bool at_latest = model->scroll_lines == 0;
    const bool at_oldest = model->scroll_lines >= ui_max_scroll(model);

    for (int index = 0; index < 3; index++) {
        const float button_x = x + (float)index * (UI_SCROLL_BUTTON + UI_SCROLL_GAP);
        const bool live = index == 0 ? !at_oldest : (index == 1 ? !at_latest : !at_latest);
        const u32 ink = live ? UI_INK : ui_alpha(UI_INK_FAINT, 130);
        ui_panel_outlined(
            button_x,
            y,
            UI_SCROLL_BUTTON,
            UI_SCROLL_BUTTON,
            2.0f,
            live ? UI_BG2 : ui_blend(UI_BG2, 130, UI_BG1),
            live ? UI_LINE : ui_blend(UI_LINE, 110, UI_BG1)
        );
        const float center_x = button_x + UI_SCROLL_BUTTON / 2.0f;
        const float center_y = y + UI_SCROLL_BUTTON / 2.0f;
        if (index == 2) {
            /* Jump to the newest line: an arrow that lands on a floor rule. */
            ui_triangle_glyph(center_x, center_y - 2.0f, 7.0f, 1, ink);
            ui_fill(center_x - 5.0f, center_y + 6.0f, 10.0f, 2.0f, ink);
        } else {
            ui_triangle_glyph(center_x, center_y, 9.0f, index == 0 ? -1 : 1, ink);
        }
    }
}

static void ui_bottom_status(const UiModel *model, float y, bool busy)
{
    ui_fill(0.0f, y, UI_BOT_W, UI_STATUS_H, UI_BG1);
    ui_fill(0.0f, y + UI_STATUS_H - 1.0f, UI_BOT_W, 1.0f, UI_LINE);

    /* The scroll cluster claims the right end, so the text stops short of it. */
    const bool scrollable = model->screen == UI_SCREEN_MAIN
        && !model->recording
        && ui_max_scroll(model) > 0;
    const float text_width = scrollable
        ? UI_BOT_W - 14.0f - UI_SCROLL_CLUSTER_W - 20.0f
        : UI_BOT_W - (busy ? 44.0f : 28.0f);

    ui_draw_clipped(
        14.0f,
        y + 3.0f,
        text_width,
        UI_SCALE_HEAD,
        UI_INK,
        C2D_AlignLeft,
        model->view_state != NULL ? model->view_state : ""
    );
    ui_draw_clipped(
        14.0f,
        y + 21.0f,
        text_width,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignLeft,
        model->detail != NULL ? model->detail : ""
    );
    ui_draw_clipped(
        14.0f,
        y + 32.0f,
        text_width,
        UI_SCALE_MICRO,
        ui_alpha(UI_INK_FAINT, 220),
        C2D_AlignLeft,
        model->detail_secondary != NULL ? model->detail_secondary : ""
    );
    if (busy && !scrollable) {
        ui_spinner(UI_BOT_W - 20.0f, y + 15.0f, 5.0f, 1.7f, UI_AZURE);
    }
    if (scrollable) {
        ui_scroll_cluster(model, y);
    }
}

/* ------------------------------------------------------------- task rail -- */

/*
 * Which slice of the task list the rail shows. Derived only from the active
 * index and the count, so the rail never depends on retained scroll state and
 * the hit test can recompute it exactly.
 */
static size_t ui_rail_first(const UiModel *model, size_t *visible)
{
    size_t shown = model->task_count < UI_RAIL_TABS
        ? model->task_count
        : UI_RAIL_TABS;
    *visible = shown;
    if (model->task_count <= UI_RAIL_TABS || !model->task_active_valid) {
        return 0;
    }
    size_t first = 0;
    if (model->task_active >= UI_RAIL_TABS) {
        first = model->task_active - UI_RAIL_TABS + 1;
    }
    if (first + UI_RAIL_TABS > model->task_count) {
        first = model->task_count - UI_RAIL_TABS;
    }
    return first;
}

static float ui_rail_tab_width(size_t visible)
{
    if (visible == 0) {
        return 0.0f;
    }
    return (UI_BOT_W - UI_RAIL_MORE_W) / (float)visible;
}

static void ui_rail_tab(
    const UiModel *model,
    const UiTask *task,
    float x,
    float width,
    bool active
)
{
    const u32 accent = ui_task_accent(task->state);
    const bool pressed = model->touch_down
        && (float)model->touch_x >= x
        && (float)model->touch_x < x + width
        && (float)model->touch_y < UI_RAIL_H;

    if (active) {
        ui_fill(x, 0.0f, width, UI_RAIL_H - 1.0f, UI_BG2);
        ui_fill(x, 0.0f, width, 2.0f, accent);
    } else if (pressed) {
        ui_fill(x, 0.0f, width, UI_RAIL_H - 1.0f, ui_blend(UI_AZURE, 30, UI_BG0));
    }
    ui_fill(x + width - 1.0f, 4.0f, 1.0f, UI_RAIL_H - 9.0f, ui_alpha(UI_LINE, 200));

    const float dot_x = x + 10.0f;
    const float dot_y = UI_RAIL_H / 2.0f;
    if (task->state == UI_TASK_WORKING) {
        ui_spinner(dot_x, dot_y, 4.0f, 1.4f, accent);
    } else {
        ui_dot(dot_x, dot_y, task->state == UI_TASK_ATTENTION ? 4.0f : 3.0f, accent);
        if (task->state == UI_TASK_ATTENTION) {
            /* A halo, so "needs you" is not carried by hue alone. */
            const float pulse = 0.5f + 0.5f * sinf(
                (float)(frame_time_ms % 1400u) / 1400.0f * 2.0f * (float)M_PI
            );
            ui_dot(dot_x, dot_y, 7.0f, ui_alpha(accent, (u8)(30.0f + 50.0f * pulse)));
        }
    }

    ui_draw_clipped(
        x + 19.0f,
        UI_RAIL_H / 2.0f - 7.0f,
        width - 25.0f - (task->unread ? 8.0f : 0.0f),
        UI_SCALE_MICRO,
        active ? UI_INK : UI_INK_DIM,
        C2D_AlignLeft,
        task->label != NULL ? task->label : "task"
    );
    if (task->unread && !active) {
        ui_dot(x + width - 8.0f, UI_RAIL_H / 2.0f - 3.0f, 2.5f, UI_AZURE);
    }
}

/*
 * The rail is the answer to "what else is running, and does any of it need me".
 * It is always one tap or one D-pad press away from another task.
 */
static void ui_task_rail(const UiModel *model)
{
    ui_fill(0.0f, 0.0f, UI_BOT_W, UI_RAIL_H, UI_BG1);
    ui_fill(0.0f, UI_RAIL_H - 1.0f, UI_BOT_W, 1.0f, UI_LINE);

    size_t visible = 0;
    const size_t first = ui_rail_first(model, &visible);
    const float tab_width = ui_rail_tab_width(visible);

    for (size_t slot = 0; slot < visible; slot++) {
        const size_t index = first + slot;
        ui_rail_tab(
            model,
            &model->tasks[index],
            (float)slot * tab_width,
            tab_width,
            model->task_active_valid && index == model->task_active
        );
    }

    if (visible == 0) {
        ui_draw(
            14.0f,
            UI_RAIL_H / 2.0f - 7.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "no other tasks"
        );
    }

    /* Manager button: a list glyph, plus a badge when something else waits. */
    const float more_x = UI_BOT_W - UI_RAIL_MORE_W;
    const bool pressed = model->touch_down
        && (float)model->touch_x >= more_x
        && (float)model->touch_y < UI_RAIL_H;
    ui_fill(
        more_x,
        0.0f,
        UI_RAIL_MORE_W,
        UI_RAIL_H - 1.0f,
        pressed ? ui_blend(UI_AZURE, 40, UI_BG1) : UI_BG1
    );
    ui_fill(more_x, 4.0f, 1.0f, UI_RAIL_H - 9.0f, ui_alpha(UI_LINE, 200));
    for (int line = 0; line < 3; line++) {
        const float line_y = 10.0f + (float)line * 5.0f;
        ui_fill(more_x + 12.0f, line_y, 2.0f, 2.0f, UI_INK_DIM);
        ui_fill(more_x + 16.0f, line_y, 9.0f, 2.0f, UI_INK_DIM);
    }
    const unsigned int waiting = ui_attention_elsewhere(model);
    if (waiting > 0) {
        ui_dot(more_x + UI_RAIL_MORE_W - 9.0f, 9.0f, 5.0f, UI_CORAL);
        ui_drawf(
            more_x + UI_RAIL_MORE_W - 9.0f,
            3.0f,
            UI_SCALE_MICRO,
            UI_BG0,
            C2D_AlignCenter,
            "%u",
            waiting > 9 ? 9u : waiting
        );
    }
}

/* ------------------------------------------------------------ action bar -- */

/*
 * Every screen answers "what can I do right now" with this list, and nothing
 * else is drawn. An action that is not currently possible is absent rather than
 * dimmed, because a permanent row of dead keys teaches the user to ignore it.
 */
static size_t ui_actions_for(const UiModel *model, UiAction *actions)
{
    size_t count = 0;

    switch (model->screen) {
        case UI_SCREEN_MAIN:
            if (model->recording) {
                break;
            }
            if (model->approval_pending) {
                actions[count++] = (UiAction){ UI_HIT_PRIMARY, "A", "Approve once", UI_CORAL };
                actions[count++] = (UiAction){ UI_HIT_BACK, "B", "Decline", UI_INK_DIM };
                break;
            }
            if (model->transcript_ready) {
                actions[count++] = (UiAction){ UI_HIT_PRIMARY, "A", "Send", UI_MINT };
                actions[count++] = (UiAction){ UI_HIT_TERTIARY, "Y", "Edit", UI_AZURE };
                actions[count++] = (UiAction){ UI_HIT_BACK, "B", "Cancel", UI_INK_DIM };
                break;
            }
            if (model->turn_active) {
                actions[count++] = (UiAction){ UI_HIT_SECONDARY, "X", "Interrupt", UI_ROSE };
                actions[count++] = (UiAction){ UI_HIT_TASK_LIST, "B", "Tasks", UI_INK_DIM };
                break;
            }
            /* Four across is 70 px each: labels have to be one short word. */
            actions[count++] = (UiAction){ UI_HIT_PRIMARY, "A", "Type", UI_MINT };
            actions[count++] = (UiAction){ UI_HIT_PHOTO, "L", "Photo", UI_AZURE };
            actions[count++] = (UiAction){ UI_HIT_SECONDARY, "X", "New", UI_VIOLET };
            actions[count++] = (UiAction){ UI_HIT_TASK_LIST, "B", "Tasks", UI_INK_DIM };
            break;

        case UI_SCREEN_PHOTO:
            if (model->photo_progress_percent != UI_PHOTO_PROGRESS_NONE) {
                break;
            }
            actions[count++] = (UiAction){ UI_HIT_PRIMARY, "A", "Attach", UI_MINT };
            actions[count++] = (UiAction){ UI_HIT_BACK, "B", "Discard", UI_INK_DIM };
            break;

        case UI_SCREEN_PAIRING:
            if (model->pairing_phase == UI_PAIRING_SUCCEEDED) {
                actions[count++] = (UiAction){ UI_HIT_PRIMARY, "A", "Continue", UI_MINT };
                actions[count++] = (UiAction){ UI_HIT_BACK, "B", "Start screen", UI_INK_DIM };
            } else if (model->pairing_phase == UI_PAIRING_FAILED) {
                actions[count++] = (UiAction){ UI_HIT_PRIMARY, "A", "Scan again", UI_AZURE };
                actions[count++] = (UiAction){ UI_HIT_TERTIARY, "Y", "Type code", UI_INK_DIM };
                actions[count++] = (UiAction){ UI_HIT_BACK, "B", "Back", UI_INK_DIM };
            } else if (model->pairing_phase == UI_PAIRING_AIMING) {
                actions[count++] = (UiAction){ UI_HIT_TERTIARY, "Y", "Type code", UI_AZURE };
                actions[count++] = (UiAction){ UI_HIT_BACK, "B", "Cancel", UI_INK_DIM };
            } else {
                actions[count++] = (UiAction){ UI_HIT_BACK, "B", "Cancel", UI_INK_DIM };
            }
            break;

        case UI_SCREEN_BOOT:
        case UI_SCREEN_HOME:
        case UI_SCREEN_SESSIONS:
        default:
            break;
    }
    return count;
}

static void ui_action_rect(
    size_t index,
    size_t count,
    float *x,
    float *y,
    float *width,
    float *height
)
{
    const float span = UI_BOT_W - 2.0f * UI_ACTION_MARGIN;
    const float each = count > 0
        ? (span - (float)(count - 1) * UI_ACTION_GAP) / (float)count
        : span;
    *x = UI_ACTION_MARGIN + (float)index * (each + UI_ACTION_GAP);
    *y = UI_ACTIONS_Y + 6.0f;
    *width = each;
    *height = UI_ACTIONS_H - 12.0f;
}

static void ui_action_bar(const UiModel *model)
{
    UiAction actions[UI_ACTIONS_MAX];
    const size_t count = ui_actions_for(model, actions);
    if (count == 0) {
        return;
    }

    for (size_t index = 0; index < count; index++) {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        ui_action_rect(index, count, &x, &y, &width, &height);

        const bool pressed = model->touch_down
            && (float)model->touch_x >= x
            && (float)model->touch_x < x + width
            && (float)model->touch_y >= y
            && (float)model->touch_y < y + height;
        const bool accented = actions[index].accent != UI_INK_DIM;

        ui_panel_outlined(
            x,
            y,
            width,
            height,
            UI_CHAMFER,
            pressed
                ? ui_blend(actions[index].accent, 70, UI_BG0)
                : (accented ? ui_blend(actions[index].accent, 30, UI_BG0) : UI_BG2),
            ui_alpha(accented ? actions[index].accent : UI_LINE, accented ? 150 : 255)
        );

        /* Key cap first: the button and the physical key are one thing. */
        const float cap = UI_KEY_CAP;
        const u32 cap_fill = ui_blend(actions[index].accent, 90, UI_BG0);
        const u32 cap_ink = accented ? actions[index].accent : UI_INK;
        const u32 label_ink = accented ? UI_INK : UI_INK_DIM;

        /*
         * Four actions across leaves 70 px each, and a cap beside the label
         * leaves under 30 px of that for the word — enough to turn "Photo" into
         * "Ph..." on the device, where the real system font is wider than the
         * host preview's estimate. Three across is only a little better. Below
         * this width the cap sits above the label instead, which gives the word
         * the whole button; two across still has room to sit side by side.
         */
        if (width < 110.0f) {
            ui_key_cap(
                x + width / 2.0f,
                y + 13.0f,
                cap,
                actions[index].cap,
                cap_fill,
                cap_ink
            );
            ui_draw_clipped(
                x + width / 2.0f,
                y + 24.0f,
                width - 8.0f,
                UI_SCALE_LABEL,
                label_ink,
                C2D_AlignCenter,
                actions[index].label
            );
            continue;
        }

        ui_key_cap(
            x + 9.0f + cap / 2.0f,
            y + height / 2.0f,
            cap,
            actions[index].cap,
            cap_fill,
            cap_ink
        );
        ui_draw_clipped(
            x + 9.0f + cap + 7.0f,
            y + height / 2.0f - 7.0f,
            width - cap - 26.0f,
            UI_SCALE_LABEL,
            label_ink,
            C2D_AlignLeft,
            actions[index].label
        );
    }
}

/*
 * List screens make their rows the targets, so they get one quiet line of key
 * hints instead of a second row of buttons competing with the list.
 */
static void ui_hint_line(float y, const char *text)
{
    ui_draw(
        UI_BOT_W / 2.0f,
        y,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignCenter,
        text
    );
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

#define UI_HERO_X       12.0f
#define UI_HERO_W       (UI_BOT_W - 24.0f)
#define UI_HERO_Y       (UI_CONTENT_Y + 4.0f)
#define UI_HERO_H       (UI_ACTIONS_Y - UI_CONTENT_Y - 8.0f)
/* Recording has no competing action, so it takes the action band as well. */
#define UI_HERO_TALL_H  (UI_BAR_Y - UI_HERO_Y - 6.0f)
/* Screens with no task rail start their single panel higher. */
#define UI_SHEET_Y      (UI_STATUS_H + 6.0f)
#define UI_SHEET_H      (UI_ACTIONS_Y - UI_SHEET_Y - 6.0f)

/*
 * Push-to-talk is also a touch target, because holding a shoulder button is
 * awkward one-handed and the stylus hand is already on this screen.
 */
static void ui_hero_push_to_talk(const UiModel *model)
{
    const float x = UI_HERO_X;
    const float y = UI_HERO_Y;
    const float width = UI_HERO_W;
    const float height = UI_HERO_H;
    const bool ready = model->microphone_ready;
    const u32 accent = ready ? UI_MINT : UI_INK_FAINT;
    const bool pressed = ready
        && model->touch_down
        && (float)model->touch_x >= x
        && (float)model->touch_x < x + width
        && (float)model->touch_y >= y
        && (float)model->touch_y < y + height;

    ui_panel_outlined(
        x,
        y,
        width,
        height,
        UI_CHAMFER,
        pressed ? ui_blend(accent, 60, UI_BG0) : UI_BG2,
        ui_alpha(accent, ready ? 90 : 50)
    );
    ui_microphone_glyph(x + 38.0f, y + height / 2.0f, accent);

    if (ready) {
        /*
         * The shoulder is named with the same cap the action bar uses, so the
         * key the sentence is talking about looks like the key on the hardware.
         */
        const float head_y = y + 14.0f;
        const float cap_width = ui_key_cap_width(UI_KEY_CAP, "R");
        float cursor = x + 74.0f;
        ui_draw(cursor, head_y, UI_SCALE_HEAD, UI_INK, C2D_AlignLeft, "HOLD");
        cursor += ui_measure("HOLD", UI_SCALE_HEAD) + 9.0f;
        ui_key_cap(
            cursor + cap_width / 2.0f,
            head_y + 8.0f,
            UI_KEY_CAP,
            "R",
            ui_blend(accent, 90, UI_BG0),
            accent
        );
        cursor += cap_width + 9.0f;
        ui_draw(cursor, head_y, UI_SCALE_HEAD, UI_INK, C2D_AlignLeft, "TO TALK");
        ui_draw(
            x + 74.0f,
            y + 38.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            "or hold this panel with the stylus"
        );
    } else {
        ui_draw(
            x + 74.0f,
            y + 14.0f,
            UI_SCALE_HEAD,
            UI_INK_DIM,
            C2D_AlignLeft,
            "MIC UNAVAILABLE"
        );
        ui_draw(
            x + 74.0f,
            y + 38.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "press A to type instead"
        );
    }
}

static void ui_hero_recording(const UiModel *model)
{
    const float x = UI_HERO_X;
    const float y = UI_HERO_Y;
    const float width = UI_HERO_W;
    const float height = UI_HERO_TALL_H;

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
        y + 24.0f,
        11.0f,
        ui_alpha(UI_ROSE, (u8)(40.0f + 60.0f * pulse))
    );
    ui_dot(x + 26.0f, y + 24.0f, 6.0f, UI_ROSE);

    char elapsed[24];
    char limit[16];
    ui_format_duration(elapsed, sizeof(elapsed), model->record_ms, true);
    ui_format_duration(limit, sizeof(limit), model->record_max_ms, false);
    ui_draw(x + 46.0f, y + 11.0f, UI_SCALE_HEAD, UI_INK, C2D_AlignLeft, elapsed);
    ui_drawf(
        x + 46.0f + ui_measure(elapsed, UI_SCALE_HEAD) + 8.0f,
        y + 17.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignLeft,
        "/ %s",
        limit
    );
    ui_draw(
        x + width - 12.0f,
        y + 11.0f,
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
    const float mid_y = y + 62.0f;
    for (unsigned int index = 0; index < UI_WAVE_BARS; index++) {
        const unsigned int slot = (wave_head + index) % UI_WAVE_BARS;
        const float level = (float)wave_levels[slot] / 100.0f;
        const float bar_height = 2.0f + level * 30.0f;
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
    ui_fill(wave_left, y + 92.0f, track_w, 2.0f, ui_alpha(UI_LINE, 220));
    ui_fill(
        wave_left,
        y + 92.0f,
        track_w * (fraction > 1.0f ? 1.0f : fraction),
        2.0f,
        UI_ROSE
    );
    ui_draw(
        x + width / 2.0f,
        y + 99.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignCenter,
        "release to send for transcription"
    );
}

static void ui_hero_working(void)
{
    const float x = UI_HERO_X;
    const float y = UI_HERO_Y;
    const float width = UI_HERO_W;
    const float height = UI_HERO_H;

    ui_panel_outlined(
        x,
        y,
        width,
        height,
        UI_CHAMFER,
        UI_BG2,
        ui_alpha(UI_AMBER, 90)
    );
    ui_spinner(x + 30.0f, y + 26.0f, 9.0f, 2.4f, UI_AMBER);
    ui_draw(
        x + 54.0f,
        y + 14.0f,
        UI_SCALE_HEAD,
        UI_INK,
        C2D_AlignLeft,
        "AGENT WORKING"
    );
    ui_draw(
        x + 54.0f,
        y + 36.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignLeft,
        "responses stream to the top screen"
    );
    ui_indeterminate_bar(x + 14.0f, y + height - 12.0f, width - 28.0f, UI_AMBER);
}

/*
 * A decision the user owes an answer to. The buttons live in the action bar,
 * so this band spends all of its room on what is actually being decided.
 */
static void ui_hero_decision(
    const char *title,
    u32 accent,
    const char *body,
    const char *fallback
)
{
    const float x = UI_HERO_X;
    const float y = UI_HERO_Y;
    const float width = UI_HERO_W;
    const float height = UI_HERO_H;

    ui_card(x, y, width, height, accent);

    ui_draw(x + 14.0f, y + 8.0f, UI_SCALE_MICRO, accent, C2D_AlignLeft, title);

    const char *text = body != NULL && body[0] != '\0' ? body : fallback;
    const UiWrapCache *wrapped = ui_wrap(
        UI_WRAP_SLOT_HERO,
        text,
        width - 28.0f,
        UI_SCALE_BODY
    );
    const size_t limit = wrapped->line_count > 3 ? 3 : wrapped->line_count;
    float line_y = y + 22.0f;
    for (size_t line = 0; line < limit; line++) {
        ui_draw_span(x + 14.0f, line_y, UI_SCALE_BODY, UI_INK, text, &wrapped->lines[line]);
        line_y += UI_BODY_LINE_H;
    }
    if (wrapped->line_count > limit) {
        ui_draw(
            x + 14.0f,
            y + height - 13.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "full request on the top screen"
        );
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
    ui_fill(0.0f, UI_CONTENT_Y, UI_BOT_W, UI_BAR_Y - UI_CONTENT_Y, UI_BG0);
    ui_task_rail(model);
    /* A pending approval is waiting on the user, not on progress. */
    ui_bottom_status(
        model,
        UI_RAIL_H,
        model->recording || (model->turn_active && !model->approval_pending)
    );

    if (model->recording) {
        ui_hero_recording(model);
    } else if (model->approval_pending) {
        ui_hero_decision(
            "APPROVAL REQUIRED",
            UI_CORAL,
            model->approval_summary,
            "The agent is asking for permission to continue."
        );
    } else if (model->transcript_ready) {
        ui_hero_decision(
            "SEND THIS TO THE AGENT?",
            UI_MINT,
            model->transcript,
            "The transcript came back empty."
        );
    } else if (model->turn_active) {
        ui_hero_working();
    } else {
        ui_hero_push_to_talk(model);
    }

    ui_action_bar(model);
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
    ui_spinner(UI_BOT_W / 2.0f, 96.0f, 12.0f, 3.0f, UI_AZURE);
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

/* ----------------------------------------------------------- start screen -- */

static void ui_render_top_home(const UiModel *model)
{
    ui_fill_vertical(0.0f, 0.0f, UI_TOP_W, UI_SCREEN_H, UI_BG1, UI_BG0);

    const float center_x = UI_TOP_W / 2.0f;
    ui_draw(center_x, 34.0f, 1.0f, UI_INK, C2D_AlignCenter, "3gent");
    ui_accent_rule(center_x - 54.0f, 66.0f, 108.0f, 2.0f);
    ui_draw(
        center_x,
        76.0f,
        UI_SCALE_MICRO,
        UI_INK_DIM,
        C2D_AlignCenter,
        "a pocket remote for coding agents"
    );

    const float x = 46.0f;
    const float y = 98.0f;
    const float width = UI_TOP_W - 2.0f * x;
    const float height = 90.0f;
    const u32 accent = model->paired ? UI_MINT : UI_AZURE;

    ui_card(x, y, width, height, accent);
    /* The rule sits at the same height either way, so the card does not jump. */
    ui_fill(x + 16.0f, y + 53.0f, width - 32.0f, 1.0f, ui_alpha(UI_INK_FAINT, 110));

    if (model->paired) {
        ui_draw(
            x + 16.0f,
            y + 12.0f,
            UI_SCALE_MICRO,
            accent,
            C2D_AlignLeft,
            "PAIRED MACHINE"
        );
        ui_draw_clipped(
            x + 16.0f,
            y + 28.0f,
            width - 32.0f,
            UI_SCALE_HEAD,
            UI_INK,
            C2D_AlignLeft,
            model->paired_bridge != NULL ? model->paired_bridge : "bridge"
        );
        ui_draw_clipped(
            x + 16.0f,
            y + 60.0f,
            width - 32.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            model->paired_endpoint != NULL ? model->paired_endpoint : ""
        );
        ui_draw_clipped(
            x + 16.0f,
            y + 73.0f,
            width - 32.0f,
            UI_SCALE_MICRO,
            ui_alpha(UI_INK_FAINT, 210),
            C2D_AlignLeft,
            model->paired_since != NULL ? model->paired_since : ""
        );
    } else {
        ui_draw(
            x + 16.0f,
            y + 12.0f,
            UI_SCALE_MICRO,
            accent,
            C2D_AlignLeft,
            "NO MACHINE PAIRED"
        );
        ui_draw_clipped(
            x + 16.0f,
            y + 30.0f,
            width - 32.0f,
            UI_SCALE_BODY,
            UI_INK,
            C2D_AlignLeft,
            "Start the bridge on your computer"
        );
        ui_draw_clipped(
            x + 16.0f,
            y + 61.0f,
            width - 32.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            "then scan the QR code it prints"
        );
    }

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

/* ------------------------------------------------------------ list rows -- */

#define UI_ROW_H       27.0f
#define UI_ROW_X       10.0f
#define UI_ROW_W       (UI_BOT_W - 2.0f * UI_ROW_X)
#define UI_ROW_TOP     (UI_STATUS_H + 4.0f)
#define UI_ROW_MAX     5u

/*
 * List rows are the touch targets on the screens that have them, which is why
 * those screens get a single hint line rather than a second bank of buttons:
 * the thing to press is already the thing you are reading.
 */
static void ui_list_row(
    const UiModel *model,
    size_t index,
    const char *label,
    const char *hint,
    bool selected,
    bool enabled,
    u32 accent
)
{
    const float y = UI_ROW_TOP + (float)index * UI_ROW_H;
    const bool pressed = enabled
        && model->touch_down
        && (float)model->touch_y >= y
        && (float)model->touch_y < y + UI_ROW_H - 2.0f
        && (float)model->touch_x >= UI_ROW_X
        && (float)model->touch_x < UI_ROW_X + UI_ROW_W;
    const u32 ink = !enabled
        ? ui_alpha(UI_INK_FAINT, 150)
        : (selected ? UI_INK : UI_INK_DIM);

    /* The panel has to clear both text lines, or it crops the hint. */
    if (selected || pressed) {
        ui_panel_outlined(
            UI_ROW_X,
            y,
            UI_ROW_W,
            UI_ROW_H - 2.0f,
            2.0f,
            ui_blend(accent, pressed ? 60 : (enabled ? 34 : 14), UI_BG0),
            ui_alpha(accent, enabled ? 150 : 60)
        );
    }
    ui_fill(
        UI_ROW_X + 3.0f,
        y + 3.0f,
        2.0f,
        UI_ROW_H - 8.0f,
        selected || pressed ? accent : UI_LINE
    );
    ui_draw_clipped(
        UI_ROW_X + 13.0f,
        y + 1.0f,
        UI_ROW_W - 26.0f,
        UI_SCALE_LABEL,
        ink,
        C2D_AlignLeft,
        label
    );
    if (hint != NULL && hint[0] != '\0') {
        ui_draw_clipped(
            UI_ROW_X + 13.0f,
            y + 13.0f,
            UI_ROW_W - 26.0f,
            UI_SCALE_MICRO,
            ui_alpha(UI_INK_FAINT, enabled ? 220 : 130),
            C2D_AlignLeft,
            hint
        );
    }
}

static void ui_render_bottom_home(const UiModel *model)
{
    ui_fill(0.0f, 0.0f, UI_BOT_W, UI_SCREEN_H, UI_BG0);
    ui_bottom_status(model, 0.0f, false);

    for (size_t index = 0;
         index < model->menu_count && index < UI_ROW_MAX;
         index++) {
        ui_list_row(
            model,
            index,
            model->menu_labels[index],
            model->menu_hints != NULL ? model->menu_hints[index] : NULL,
            index == model->menu_selected,
            model->menu_enabled == NULL || model->menu_enabled[index],
            UI_AZURE
        );
    }

    /* START is worth naming here and nowhere else: this is the only way out. */
    ui_hint_line(UI_BAR_Y - 13.0f, "Up/Down choose      A select      START exit");
    ui_bottom_bar(model);
}

/* ------------------------------------------------------------- pairing -- */

/* Corner brackets rather than a full frame: they mark the target without
 * hiding the QR the user is trying to fill it with. */
static void ui_pairing_reticle(void)
{
    const float size = 132.0f;
    const float left = (UI_TOP_W - size) / 2.0f;
    const float top = (UI_SCREEN_H - size) / 2.0f - 6.0f;
    const float arm = 26.0f;
    const float thickness = 3.0f;
    const u32 color = ui_alpha(UI_MINT, 220);

    const float corners[4][2] = {
        { left, top },
        { left + size - arm, top },
        { left, top + size - thickness },
        { left + size - arm, top + size - thickness },
    };
    for (int index = 0; index < 4; index++) {
        ui_fill(corners[index][0], corners[index][1], arm, thickness, color);
    }
    const float verticals[4][2] = {
        { left, top },
        { left + size - thickness, top },
        { left, top + size - arm },
        { left + size - thickness, top + size - arm },
    };
    for (int index = 0; index < 4; index++) {
        ui_fill(verticals[index][0], verticals[index][1], thickness, arm, color);
    }
}

static void ui_render_top_pairing(const UiModel *model)
{
    ui_fill(0.0f, 0.0f, UI_TOP_W, UI_SCREEN_H, UI_BG0);

    if (model->pairing_preview_ready && photo_texture_ready) {
        C2D_DrawImageAt(photo_image, 0.0f, 0.0f, 0.0f, NULL, 1.0f, 1.0f);
    } else {
        ui_draw(
            UI_TOP_W / 2.0f,
            104.0f,
            UI_SCALE_BODY,
            UI_INK_FAINT,
            C2D_AlignCenter,
            "Opening the camera..."
        );
        ui_spinner(UI_TOP_W / 2.0f, 132.0f, 10.0f, 2.6f, UI_AZURE);
    }

    if (model->pairing_phase == UI_PAIRING_AIMING) {
        ui_pairing_reticle();
    } else {
        const u32 accent = model->pairing_phase == UI_PAIRING_FAILED
            ? UI_ROSE
            : (model->pairing_phase == UI_PAIRING_SUCCEEDED ? UI_MINT : UI_AZURE);
        const char *title = "READING PAIRING CODE";
        if (model->pairing_phase == UI_PAIRING_EXCHANGING) {
            title = "ASKING THE BRIDGE TO PAIR";
        } else if (model->pairing_phase == UI_PAIRING_SUCCEEDED) {
            title = "PAIRED";
        } else if (model->pairing_phase == UI_PAIRING_FAILED) {
            title = "PAIRING FAILED";
        }
        /*
         * `ui_attention_card` dims from the header band down, because every
         * other screen has a header there. This one is full-bleed camera, so
         * the same scrim has to cover the top strip too.
         */
        /*
         * `ui_attention_card` dims from the header band down to the footer,
         * because every other screen has chrome there. This one is full-bleed
         * camera, so both strips have to be dimmed as well or the viewfinder
         * shows through undimmed above and below the card.
         */
        ui_fill(0.0f, 0.0f, UI_TOP_W, UI_HEADER_H, ui_alpha(UI_BG0, 236));
        ui_fill(
            0.0f,
            UI_FOOTER_Y,
            UI_TOP_W,
            UI_SCREEN_H - UI_FOOTER_Y,
            ui_alpha(UI_BG0, 236)
        );
        /*
         * No key hint here. The bottom screen carries the live actions for this
         * phase, and a second, differently worded list on the top screen is how
         * the two end up disagreeing.
         */
        ui_attention_card(
            title,
            accent,
            model->pairing_message != NULL ? model->pairing_message : "",
            ""
        );
        return;
    }

    /* Aiming only: the scrim explains the viewfinder it is drawn over. */
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
        UI_TOP_W - 2.0f * UI_PAD - 70.0f,
        UI_SCALE_MICRO,
        UI_INK,
        C2D_AlignLeft,
        "Fill the brackets with the QR code on your computer"
    );
    ui_drawf(
        UI_TOP_W - UI_PAD,
        UI_SCREEN_H - 20.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignRight,
        "%u looks",
        model->pairing_frames_examined
    );
}

static void ui_render_bottom_pairing(const UiModel *model)
{
    ui_fill(0.0f, UI_STATUS_H, UI_BOT_W, UI_BAR_Y - UI_STATUS_H, UI_BG0);
    ui_bottom_status(
        model,
        0.0f,
        model->pairing_phase == UI_PAIRING_EXCHANGING
            || model->pairing_phase == UI_PAIRING_DECODED
    );

    const float x = UI_HERO_X;
    const float y = UI_SHEET_Y;
    const float width = UI_HERO_W;
    const float height = UI_SHEET_H;
    const u32 accent = model->pairing_phase == UI_PAIRING_FAILED
        ? UI_ROSE
        : (model->pairing_phase == UI_PAIRING_SUCCEEDED ? UI_MINT : UI_AZURE);

    ui_panel_outlined(x, y, width, height, UI_CHAMFER, UI_BG2, ui_alpha(accent, 110));

    if (model->pairing_phase == UI_PAIRING_SUCCEEDED) {
        ui_draw(x + 16.0f, y + 14.0f, UI_SCALE_HEAD, UI_MINT, C2D_AlignLeft, "Paired");
        ui_draw_clipped(
            x + 16.0f,
            y + 40.0f,
            width - 32.0f,
            UI_SCALE_BODY,
            UI_INK,
            C2D_AlignLeft,
            model->pairing_bridge != NULL ? model->pairing_bridge : ""
        );
        ui_draw(
            x + 16.0f,
            y + 62.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "The device key is saved on the SD card"
        );
    } else if (model->pairing_phase == UI_PAIRING_FAILED) {
        ui_draw(x + 16.0f, y + 14.0f, UI_SCALE_HEAD, UI_ROSE, C2D_AlignLeft, "Not paired");
        ui_draw_clipped(
            x + 16.0f,
            y + 40.0f,
            width - 32.0f,
            UI_SCALE_BODY,
            UI_INK,
            C2D_AlignLeft,
            model->pairing_message != NULL ? model->pairing_message : ""
        );
        ui_draw(
            x + 16.0f,
            y + 62.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "The code expires; ask the bridge for a new one"
        );
    } else if (model->pairing_phase == UI_PAIRING_AIMING) {
        ui_draw(
            x + 16.0f,
            y + 14.0f,
            UI_SCALE_HEAD,
            UI_INK,
            C2D_AlignLeft,
            "Looking for a QR code"
        );
        ui_draw(
            x + 16.0f,
            y + 40.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            "Hold the handheld steady, about 20 cm away"
        );
        ui_indeterminate_bar(x + 16.0f, y + height - 20.0f, width - 32.0f, UI_AZURE);
    } else {
        ui_spinner(x + 34.0f, y + 36.0f, 9.0f, 2.4f, UI_AZURE);
        ui_draw(
            x + 58.0f,
            y + 24.0f,
            UI_SCALE_HEAD,
            UI_INK,
            C2D_AlignLeft,
            "Exchanging the code"
        );
        ui_draw_clipped(
            x + 58.0f,
            y + 48.0f,
            width - 74.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            "The bridge issues a revocable device key"
        );
    }

    ui_action_bar(model);
    ui_bottom_bar(model);
}

/* ---------------------------------------------------------- task chooser -- */

/*
 * The manager puts the list on the bottom screen because that is the screen a
 * finger can reach, and spends the top screen on the one task the user is
 * currently pointing at. Two screens, two jobs.
 */
#define UI_TASK_ROWS_MAX 4u

/*
 * "Start a new task" is row `task_count`: one past the end of the list, and the
 * row the selection lands on when the user walks off the bottom of it.
 */
static bool ui_task_new_row_visible(const UiModel *model)
{
    return !model->tasks_retryable;
}

static bool ui_task_new_row_selected(const UiModel *model)
{
    return ui_task_new_row_visible(model)
        && model->task_selected >= model->task_count;
}

static size_t ui_task_list_first(const UiModel *model, size_t *visible)
{
    const size_t shown = model->task_count < UI_TASK_ROWS_MAX
        ? model->task_count
        : UI_TASK_ROWS_MAX;
    *visible = shown;
    if (model->task_count <= UI_TASK_ROWS_MAX) {
        return 0;
    }
    /*
     * Selecting the new-task row keeps the window where the end of the list is,
     * so stepping past the last task does not scroll anything out from under it.
     */
    const size_t anchor = model->task_selected < model->task_count
        ? model->task_selected
        : model->task_count - 1;
    size_t first = 0;
    if (anchor >= UI_TASK_ROWS_MAX) {
        first = anchor - UI_TASK_ROWS_MAX + 1;
    }
    if (first + UI_TASK_ROWS_MAX > model->task_count) {
        first = model->task_count - UI_TASK_ROWS_MAX;
    }
    return first;
}

static void ui_render_top_sessions(const UiModel *model)
{
    ui_fill(0.0f, UI_HEADER_H, UI_TOP_W, UI_SCREEN_H - UI_HEADER_H, UI_BG0);
    ui_top_header(model, "tasks on this machine");

    if (model->task_count == 0) {
        const char *headline = "No recent tasks were returned";
        const char *hint = "Start a new one to give the agent something to do";
        if (model->tasks_loading) {
            headline = "Looking for recent tasks...";
            hint = "";
        } else if (model->tasks_retryable) {
            headline = "The bridge did not answer";
            hint = "Check that it is running, then retry with A";
        }
        if (model->tasks_loading) {
            ui_spinner(UI_TOP_W / 2.0f, 100.0f, 9.0f, 2.4f, UI_AZURE);
        }
        ui_draw(
            UI_TOP_W / 2.0f,
            124.0f,
            UI_SCALE_BODY,
            model->tasks_retryable ? UI_ROSE : UI_INK_FAINT,
            C2D_AlignCenter,
            headline
        );
        ui_draw(
            UI_TOP_W / 2.0f,
            146.0f,
            UI_SCALE_MICRO,
            ui_alpha(UI_INK_FAINT, 190),
            C2D_AlignCenter,
            hint
        );
        return;
    }

    /* One line of arithmetic the user would otherwise do by eye. */
    unsigned int waiting = 0;
    unsigned int working = 0;
    for (size_t index = 0; index < model->task_count; index++) {
        if (model->tasks[index].state == UI_TASK_ATTENTION) {
            waiting++;
        } else if (model->tasks[index].state == UI_TASK_WORKING) {
            working++;
        }
    }
    ui_drawf(
        UI_PAD,
        UI_HEADER_H + 8.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignLeft,
        "%u TASK%s",
        (unsigned int)model->task_count,
        model->task_count == 1 ? "" : "S"
    );
    if (waiting > 0) {
        ui_drawf(
            UI_TOP_W - UI_PAD,
            UI_HEADER_H + 8.0f,
            UI_SCALE_MICRO,
            UI_CORAL,
            C2D_AlignRight,
            "%u waiting on you",
            waiting
        );
    } else if (working > 0) {
        ui_drawf(
            UI_TOP_W - UI_PAD,
            UI_HEADER_H + 8.0f,
            UI_SCALE_MICRO,
            UI_AMBER,
            C2D_AlignRight,
            "%u working",
            working
        );
    }

    const float x = 24.0f;
    const float y = UI_HEADER_H + 26.0f;
    const float width = UI_TOP_W - 2.0f * x;
    const float height = 108.0f;

    /* The last row is not a task, so the detail card describes what it does. */
    if (ui_task_new_row_selected(model)) {
        ui_card(x, y, width, height, UI_VIOLET);
        ui_draw(
            x + 16.0f,
            y + 12.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "NEW TASK"
        );
        ui_draw(
            x + 16.0f,
            y + 34.0f,
            UI_SCALE_BODY,
            UI_INK,
            C2D_AlignLeft,
            "Start a fresh task on the bridge"
        );
        ui_fill(
            x + 16.0f,
            y + height - 34.0f,
            width - 32.0f,
            1.0f,
            ui_alpha(UI_LINE, 220)
        );
        ui_draw(
            x + 16.0f,
            y + height - 26.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            "It opens straight away, with nothing said yet"
        );
        ui_draw(
            x + 16.0f,
            y + height - 15.0f,
            UI_SCALE_MICRO,
            ui_alpha(UI_INK_FAINT, 210),
            C2D_AlignLeft,
            "A starts it; X starts one from anywhere in this list"
        );
        return;
    }

    const size_t selected = model->task_selected < model->task_count
        ? model->task_selected
        : 0;
    const UiTask *task = &model->tasks[selected];
    const u32 accent = ui_task_accent(task->state);

    ui_card(x, y, width, height, accent);

    ui_drawf(
        x + 16.0f,
        y + 12.0f,
        UI_SCALE_MICRO,
        UI_INK_FAINT,
        C2D_AlignLeft,
        "TASK %u OF %u",
        (unsigned int)(selected + 1),
        (unsigned int)model->task_count
    );
    const UiAgentBadge badge = {
        ui_task_state_word(task->state),
        accent,
        task->state == UI_TASK_WORKING,
    };
    ui_badge(x + width - 14.0f, y + 9.0f, &badge);

    const UiWrapCache *wrapped = ui_wrap(
        UI_WRAP_SLOT_CARD,
        task->label != NULL ? task->label : "",
        width - 32.0f,
        UI_SCALE_BODY
    );
    const size_t limit = wrapped->line_count > 3 ? 3 : wrapped->line_count;
    float line_y = y + 34.0f;
    for (size_t line = 0; line < limit; line++) {
        ui_draw_span(
            x + 16.0f,
            line_y,
            UI_SCALE_BODY,
            UI_INK,
            task->label,
            &wrapped->lines[line]
        );
        line_y += UI_BODY_LINE_H;
    }

    const char *note = "Ready for a new prompt";
    if (task->state == UI_TASK_ATTENTION) {
        note = "Blocked: this task is waiting for your approval";
    } else if (task->state == UI_TASK_WORKING) {
        note = "The agent is working on this one right now";
    } else if (task->state == UI_TASK_FAILED) {
        note = "The last turn ended in an error";
    } else if (task->unread) {
        note = "New output arrived while you were elsewhere";
    }
    ui_fill(x + 16.0f, y + height - 34.0f, width - 32.0f, 1.0f, ui_alpha(UI_LINE, 220));
    ui_draw_clipped(
        x + 16.0f,
        y + height - 26.0f,
        width - 32.0f,
        UI_SCALE_MICRO,
        task->state == UI_TASK_ATTENTION ? UI_CORAL : UI_INK_DIM,
        C2D_AlignLeft,
        note
    );
    if (model->task_active_valid && selected == model->task_active) {
        ui_draw(
            x + 16.0f,
            y + height - 15.0f,
            UI_SCALE_MICRO,
            ui_alpha(UI_INK_FAINT, 210),
            C2D_AlignLeft,
            "This is the task you are already reading"
        );
    }
}

static void ui_render_bottom_sessions(const UiModel *model)
{
    ui_fill(0.0f, UI_STATUS_H, UI_BOT_W, UI_BAR_Y - UI_STATUS_H, UI_BG0);
    ui_bottom_status(model, 0.0f, model->tasks_loading);

    size_t visible = 0;
    const size_t first = ui_task_list_first(model, &visible);

    for (size_t slot = 0; slot < visible; slot++) {
        const size_t index = first + slot;
        const UiTask *task = &model->tasks[index];
        char hint[64];
        snprintf(
            hint,
            sizeof(hint),
            "%s%s%s",
            ui_task_state_word(task->state),
            model->task_active_valid && index == model->task_active
                ? "  ·  open"
                : "",
            task->unread ? "  ·  new output" : ""
        );
        ui_list_row(
            model,
            slot,
            task->label != NULL ? task->label : "task",
            hint,
            index == model->task_selected,
            true,
            ui_task_accent(task->state)
        );
    }

    /*
     * A row rather than a button, so starting a task is reachable the same way
     * as opening one: the D-pad walks onto it and A takes it. X remains the
     * shortcut for anyone using the keys. It is absent while the bridge is
     * unreachable, because starting a task would fail the same way listing them
     * just did, and retrying is the only useful move.
     */
    if (ui_task_new_row_visible(model)) {
        ui_list_row(
            model,
            visible,
            "+  Start a new task",
            "X   fresh Codex task in the bridge workspace",
            ui_task_new_row_selected(model),
            !model->tasks_loading,
            UI_VIOLET
        );
    }

    /* Shares the hint line's row: the hint is centred, this is flush right. */
    if (model->task_count > visible) {
        ui_drawf(
            UI_BOT_W - 12.0f,
            UI_BAR_Y - 13.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignRight,
            "+%u more",
            (unsigned int)(model->task_count - visible)
        );
    }

    /* A does whichever of the two things the selected row actually is. */
    ui_hint_line(
        UI_BAR_Y - 13.0f,
        model->tasks_retryable
            ? "A retries      B back to the start screen"
            : (ui_task_new_row_selected(model)
                ? "Up/Down choose      A start it      B back"
                : "Up/Down choose      A open      B back")
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
    ui_fill(0.0f, UI_STATUS_H, UI_BOT_W, UI_BAR_Y - UI_STATUS_H, UI_BG0);
    ui_bottom_status(
        model,
        0.0f,
        model->photo_progress_percent != UI_PHOTO_PROGRESS_NONE
    );

    const float x = UI_HERO_X;
    const float y = UI_SHEET_Y;
    const float width = UI_HERO_W;
    const float height = UI_SHEET_H;

    if (model->photo_progress_percent != UI_PHOTO_PROGRESS_NONE) {
        ui_panel_outlined(
            x,
            y,
            width,
            height,
            UI_CHAMFER,
            UI_BG2,
            ui_alpha(UI_AZURE, 90)
        );
        /* The status band already names the operation; this says how far it is. */
        ui_draw(
            x + 16.0f,
            y + 22.0f,
            UI_SCALE_HEAD,
            UI_INK,
            C2D_AlignLeft,
            "Sending to the bridge"
        );
        ui_drawf(
            x + width - 16.0f,
            y + 22.0f,
            UI_SCALE_HEAD,
            UI_AZURE,
            C2D_AlignRight,
            "%u%%",
            model->photo_progress_percent
        );
        const float track_x = x + 16.0f;
        const float track_w = width - 32.0f;
        ui_fill(track_x, y + 52.0f, track_w, 3.0f, ui_alpha(UI_LINE, 220));
        ui_fill(
            track_x,
            y + 52.0f,
            track_w * (float)model->photo_progress_percent / 100.0f,
            3.0f,
            UI_AZURE
        );
        ui_draw(
            x + 16.0f,
            y + 66.0f,
            UI_SCALE_MICRO,
            UI_INK_FAINT,
            C2D_AlignLeft,
            "192 000 bytes over the shared media link"
        );
    } else {
        ui_panel_outlined(
            x,
            y,
            width,
            height,
            UI_CHAMFER,
            UI_BG2,
            ui_alpha(UI_AZURE, 90)
        );
        ui_draw(
            x + 16.0f,
            y + 18.0f,
            UI_SCALE_HEAD,
            UI_INK,
            C2D_AlignLeft,
            "Attach this shot?"
        );
        ui_draw(
            x + 16.0f,
            y + 44.0f,
            UI_SCALE_MICRO,
            UI_INK_DIM,
            C2D_AlignLeft,
            "It rides along with your next prompt, then is consumed."
        );
        ui_draw(
            x + 16.0f,
            y + 60.0f,
            UI_SCALE_MICRO,
            ui_alpha(UI_INK_FAINT, 220),
            C2D_AlignLeft,
            "Only one photo is held per task."
        );
    }

    ui_action_bar(model);
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
    if (rgb565 == NULL
        || width == 0
        || height == 0
        || width > UI_PHOTO_TEX_W
        || height > UI_PHOTO_TEX_H) {
        ui_photo_preview_clear();
        return false;
    }
    /*
     * The pairing viewfinder calls this every delivered frame, so reuse the
     * existing texture and rewrite its pixels rather than freeing and
     * reallocating 256 KiB of VRAM thirty times a second.
     */
    if (!photo_texture_ready) {
        if (!C3D_TexInit(
                &photo_texture,
                (u16)UI_PHOTO_TEX_W,
                (u16)UI_PHOTO_TEX_H,
                GPU_RGB565
            )) {
            return false;
        }
        C3D_TexSetFilter(&photo_texture, GPU_LINEAR, GPU_NEAREST);
        memset(
            photo_texture.data,
            0,
            (size_t)UI_PHOTO_TEX_W * UI_PHOTO_TEX_H * 2u
        );
    }

    u16 *destination = (u16 *)photo_texture.data;
    const u16 *source = (const u16 *)(const void *)rgb565;
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

size_t ui_page_lines(const UiModel *model)
{
    float text_top = 0.0f;
    size_t visible_lines = 0;
    ui_body_layout(model, &text_top, &visible_lines);
    /* Keep one line of overlap so a paged jump has something to read back to. */
    return visible_lines > 1 ? visible_lines - 1 : 1;
}

/* ------------------------------------------------------------- hit test -- */

static bool ui_within(
    unsigned int x,
    unsigned int y,
    float left,
    float top,
    float width,
    float height
)
{
    return (float)x >= left && (float)x < left + width
        && (float)y >= top && (float)y < top + height;
}

static UiHit ui_hit_actions(const UiModel *model, unsigned int x, unsigned int y)
{
    UiAction actions[UI_ACTIONS_MAX];
    const size_t count = ui_actions_for(model, actions);
    for (size_t index = 0; index < count; index++) {
        float rect_x = 0.0f;
        float rect_y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        ui_action_rect(index, count, &rect_x, &rect_y, &width, &height);
        if (ui_within(x, y, rect_x, rect_y, width, height)) {
            return (UiHit){ actions[index].hit, 0 };
        }
    }
    return (UiHit){ UI_HIT_NONE, 0 };
}

/* Which on-screen row a point falls in, or `rows` when it falls outside. */
static size_t ui_hit_row(unsigned int x, unsigned int y, size_t rows)
{
    if ((float)x < UI_ROW_X || (float)x >= UI_ROW_X + UI_ROW_W) {
        return rows;
    }
    for (size_t index = 0; index < rows; index++) {
        const float top = UI_ROW_TOP + (float)index * UI_ROW_H;
        if (ui_within(x, y, UI_ROW_X, top, UI_ROW_W, UI_ROW_H - 2.0f)) {
            return index;
        }
    }
    return rows;
}

UiHit ui_hit_test(const UiModel *model, unsigned int x, unsigned int y)
{
    const UiHit none = { UI_HIT_NONE, 0 };
    if (model == NULL) {
        return none;
    }

    switch (model->screen) {
        case UI_SCREEN_MAIN: {
            if (model->recording) {
                /* Everything is the talk button while the microphone is open. */
                return (UiHit){ UI_HIT_TALK, 0 };
            }
            if ((float)y < UI_RAIL_H) {
                if ((float)x >= UI_BOT_W - UI_RAIL_MORE_W) {
                    return (UiHit){ UI_HIT_TASK_LIST, 0 };
                }
                size_t visible = 0;
                const size_t first = ui_rail_first(model, &visible);
                const float tab_width = ui_rail_tab_width(visible);
                if (tab_width > 0.0f) {
                    const size_t slot = (size_t)((float)x / tab_width);
                    if (slot < visible) {
                        return (UiHit){ UI_HIT_TASK, first + slot };
                    }
                }
                return none;
            }
            if (ui_max_scroll(model) > 0
                && (float)y >= UI_RAIL_H
                && (float)y < UI_RAIL_H + UI_STATUS_H) {
                const float cluster_x = ui_scroll_cluster_x();
                for (int index = 0; index < 3; index++) {
                    const float button_x =
                        cluster_x + (float)index * (UI_SCROLL_BUTTON + UI_SCROLL_GAP);
                    if (ui_within(
                            x,
                            y,
                            button_x,
                            UI_RAIL_H + (UI_STATUS_H - UI_SCROLL_BUTTON) / 2.0f,
                            UI_SCROLL_BUTTON,
                            UI_SCROLL_BUTTON
                        )) {
                        return (UiHit){
                            index == 0 ? UI_HIT_SCROLL_BACK
                                : (index == 1 ? UI_HIT_SCROLL_FORWARD
                                              : UI_HIT_SCROLL_LATEST),
                            0,
                        };
                    }
                }
                return none;
            }
            /* Holding the idle hero is the stylus equivalent of holding R. */
            if (!model->approval_pending
                && !model->transcript_ready
                && !model->turn_active
                && model->microphone_ready
                && ui_within(x, y, UI_HERO_X, UI_HERO_Y, UI_HERO_W, UI_HERO_H)) {
                return (UiHit){ UI_HIT_TALK, 0 };
            }
            return ui_hit_actions(model, x, y);
        }

        case UI_SCREEN_HOME: {
            const size_t rows = model->menu_count < UI_ROW_MAX
                ? model->menu_count
                : UI_ROW_MAX;
            const size_t row = ui_hit_row(x, y, rows);
            if (row < rows
                && (model->menu_enabled == NULL || model->menu_enabled[row])) {
                return (UiHit){ UI_HIT_MENU_ROW, row };
            }
            return none;
        }

        case UI_SCREEN_SESSIONS: {
            size_t visible = 0;
            const size_t first = ui_task_list_first(model, &visible);
            const bool can_start =
                ui_task_new_row_visible(model) && !model->tasks_loading;
            const size_t row = ui_hit_row(x, y, visible + (can_start ? 1 : 0));
            if (row < visible) {
                return (UiHit){ UI_HIT_MENU_ROW, first + row };
            }
            if (row == visible && can_start) {
                return (UiHit){ UI_HIT_SECONDARY, 0 };
            }
            return none;
        }

        case UI_SCREEN_PHOTO:
        case UI_SCREEN_PAIRING:
            return ui_hit_actions(model, x, y);

        case UI_SCREEN_BOOT:
        default:
            return none;
    }
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
        case UI_SCREEN_HOME:
            ui_render_top_home(model);
            break;
        case UI_SCREEN_PAIRING:
            ui_render_top_pairing(model);
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
        case UI_SCREEN_HOME:
            ui_render_bottom_home(model);
            break;
        case UI_SCREEN_PAIRING:
            ui_render_bottom_pairing(model);
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
