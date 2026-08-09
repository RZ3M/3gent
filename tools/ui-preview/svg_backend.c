/*
 * Records the draw calls `client-3ds/source/ui.c` makes and re-emits them as
 * SVG so the handheld layout can be reviewed on a laptop.
 *
 * This is a design tool, not a metrics oracle. The 3DS system font is not
 * available off-device, so glyph advances come from the approximation table
 * below. Text boxes are emitted with an explicit `textLength`, which means the
 * preview reproduces the widths `ui.c` computed rather than the browser's.
 */
#include "svg_backend.h"

#include <citro2d.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_CAPACITY (64 * 1024)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

struct C3D_RenderTarget_tag {
    int index;
};

struct C2D_TextBuf_s {
    char arena[ARENA_CAPACITY];
    size_t used;
};

static struct C3D_RenderTarget_tag targets[2] = { { 0 }, { 1 } };
static Buffer screens[2];
static Buffer defs;
static int active_screen;
static unsigned int gradient_serial;
static u64 simulated_time_ms;

/* Approximate 3DS system-font advances, in the font's 30 px design units. */
static const u8 advance_table[0x60] = {
     8,  7, 10, 15, 14, 20, 17,  5,  8,  8, 11, 15,  6,  9,  6, 10,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  6,  6, 14, 14, 14, 12,
    22, 17, 16, 17, 18, 15, 14, 18, 18,  7, 12, 16, 14, 22, 18, 19,
    15, 19, 16, 15, 15, 18, 17, 24, 16, 15, 15,  8, 10,  8, 12, 12,
     8, 13, 14, 12, 14, 13,  8, 14, 14,  6,  6, 13,  6, 21, 14, 14,
    14, 14,  9, 11,  9, 14, 13, 19, 13, 13, 11,  9,  6,  9, 14, 13,
};

static charWidthInfo_s width_info;

/* ------------------------------------------------------------------ buffer -- */

static void buffer_reset(Buffer *buffer)
{
    buffer->length = 0;
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

static void buffer_append(Buffer *buffer, const char *format, ...)
{
    va_list probe;
    va_start(probe, format);
    va_list measure;
    va_copy(measure, probe);
    const int needed = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    if (needed < 0) {
        va_end(probe);
        return;
    }

    const size_t required = buffer->length + (size_t)needed + 1;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 8192 : buffer->capacity;
        while (capacity < required) {
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) {
            va_end(probe);
            return;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format, probe);
    va_end(probe);
    buffer->length += (size_t)needed;
}

/* ------------------------------------------------------------------ colour -- */

static void color_parts(u32 color, unsigned int *red, unsigned int *green,
    unsigned int *blue, float *opacity)
{
    *red = color & 0xFFu;
    *green = (color >> 8) & 0xFFu;
    *blue = (color >> 16) & 0xFFu;
    *opacity = (float)((color >> 24) & 0xFFu) / 255.0f;
}

static void append_solid_paint(Buffer *buffer, u32 color)
{
    unsigned int red, green, blue;
    float opacity;
    color_parts(color, &red, &green, &blue, &opacity);
    buffer_append(
        buffer,
        " fill=\"rgb(%u,%u,%u)\" fill-opacity=\"%.3f\"",
        red, green, blue, opacity
    );
}

static void append_gradient_stop(float offset, u32 color)
{
    unsigned int red, green, blue;
    float opacity;
    color_parts(color, &red, &green, &blue, &opacity);
    buffer_append(
        &defs,
        "    <stop offset=\"%.0f%%\" stop-color=\"rgb(%u,%u,%u)\""
        " stop-opacity=\"%.3f\"/>\n",
        offset * 100.0f, red, green, blue, opacity
    );
}

static unsigned int declare_gradient(u32 from, u32 to, bool horizontal)
{
    const unsigned int identifier = gradient_serial++;
    buffer_append(
        &defs,
        "  <linearGradient id=\"g%u\" x1=\"0\" y1=\"0\" x2=\"%d\" y2=\"%d\">\n",
        identifier,
        horizontal ? 1 : 0,
        horizontal ? 0 : 1
    );
    append_gradient_stop(0.0f, from);
    append_gradient_stop(1.0f, to);
    buffer_append(&defs, "  </linearGradient>\n");
    return identifier;
}

/* -------------------------------------------------------------- lifecycle -- */

void gfxInitDefault(void) {}
void gfxExit(void) {}

u64 osGetTime(void)
{
    return simulated_time_ms;
}

void preview_set_time(u64 milliseconds)
{
    simulated_time_ms = milliseconds;
}

bool C3D_Init(size_t commandBufferSize)
{
    (void)commandBufferSize;
    return true;
}

void C3D_Fini(void) {}

bool C3D_FrameBegin(u8 flags)
{
    (void)flags;
    buffer_reset(&screens[0]);
    buffer_reset(&screens[1]);
    buffer_reset(&defs);
    /*
     * gradient_serial deliberately keeps counting across frames. Several
     * documents get inlined into one contact-sheet page, and duplicate SVG
     * element ids there would make every gradient resolve to the first one.
     */
    return true;
}

void C3D_FrameEnd(u32 flags)
{
    (void)flags;
}

bool C2D_Init(size_t maxObjects)
{
    (void)maxObjects;
    return true;
}

void C2D_Fini(void) {}
void C2D_Prepare(void) {}

C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side)
{
    (void)side;
    return &targets[screen == GFX_TOP ? 0 : 1];
}

void C2D_TargetClear(C3D_RenderTarget *target, u32 color)
{
    const int index = target->index;
    buffer_append(
        &screens[index],
        "    <rect x=\"0\" y=\"0\" width=\"%d\" height=\"240\"",
        index == 0 ? 400 : 320
    );
    append_solid_paint(&screens[index], color);
    buffer_append(&screens[index], "/>\n");
}

void C2D_SceneBegin(C3D_RenderTarget *target)
{
    active_screen = target->index;
}

/* ---------------------------------------------------------------- textures -- */

bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format)
{
    tex->width = width;
    tex->height = height;
    tex->format = format;
    tex->data = calloc((size_t)width * height, 2);
    return tex->data != NULL;
}

void C3D_TexSetFilter(
    C3D_Tex *tex,
    GPU_TEXTURE_FILTER_PARAM magFilter,
    GPU_TEXTURE_FILTER_PARAM minFilter
)
{
    (void)tex;
    (void)magFilter;
    (void)minFilter;
}

void C3D_TexFlush(C3D_Tex *tex)
{
    (void)tex;
}

void C3D_TexDelete(C3D_Tex *tex)
{
    free(tex->data);
    tex->data = NULL;
}

/* ---------------------------------------------------------------- drawing -- */

bool C2D_DrawRectangle(
    float x, float y, float z, float w, float h,
    u32 clr0, u32 clr1, u32 clr2, u32 clr3
)
{
    (void)z;
    Buffer *buffer = &screens[active_screen];
    buffer_append(
        buffer,
        "    <rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\"",
        x, y, w, h
    );
    if (clr0 == clr1 && clr1 == clr2 && clr2 == clr3) {
        append_solid_paint(buffer, clr0);
    } else if (clr0 == clr1 && clr2 == clr3) {
        buffer_append(buffer, " fill=\"url(#g%u)\"", declare_gradient(clr0, clr2, false));
    } else {
        buffer_append(buffer, " fill=\"url(#g%u)\"", declare_gradient(clr0, clr1, true));
    }
    buffer_append(buffer, "/>\n");
    return true;
}

bool C2D_DrawTriangle(
    float x0, float y0, u32 clr0,
    float x1, float y1, u32 clr1,
    float x2, float y2, u32 clr2,
    float depth
)
{
    (void)clr1;
    (void)clr2;
    (void)depth;
    Buffer *buffer = &screens[active_screen];
    buffer_append(
        buffer,
        "    <polygon points=\"%.2f,%.2f %.2f,%.2f %.2f,%.2f\"",
        x0, y0, x1, y1, x2, y2
    );
    append_solid_paint(buffer, clr0);
    buffer_append(buffer, "/>\n");
    return true;
}

bool C2D_DrawEllipse(
    float x, float y, float z, float w, float h,
    u32 clr0, u32 clr1, u32 clr2, u32 clr3
)
{
    (void)z;
    (void)clr1;
    (void)clr2;
    (void)clr3;
    Buffer *buffer = &screens[active_screen];
    buffer_append(
        buffer,
        "    <ellipse cx=\"%.2f\" cy=\"%.2f\" rx=\"%.2f\" ry=\"%.2f\"",
        x + w / 2.0f, y + h / 2.0f, w / 2.0f, h / 2.0f
    );
    append_solid_paint(buffer, clr0);
    buffer_append(buffer, "/>\n");
    return true;
}

bool C2D_DrawImage(C2D_Image img, const void *params, const void *tint)
{
    (void)img;
    (void)params;
    (void)tint;
    return true;
}

bool C2D_DrawImageAt(
    C2D_Image img,
    float x,
    float y,
    float depth,
    const void *tint,
    float scaleX,
    float scaleY
)
{
    (void)depth;
    (void)tint;
    Buffer *buffer = &screens[active_screen];
    const float width = (float)img.subtex->width * scaleX;
    const float height = (float)img.subtex->height * scaleY;
    buffer_append(
        buffer,
        "    <rect x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\""
        " fill=\"rgb(38,44,58)\" stroke=\"rgb(90,102,128)\""
        " stroke-dasharray=\"4 4\"/>\n"
        "    <text x=\"%.2f\" y=\"%.2f\" text-anchor=\"middle\""
        " font-family=\"Menlo, monospace\" font-size=\"11\""
        " fill=\"rgb(140,152,178)\">camera frame</text>\n",
        x, y, width, height,
        x + width / 2.0f, y + height / 2.0f
    );
    return true;
}

/* ------------------------------------------------------------------- text -- */

static struct C2D_TextBuf_s text_storage;

C2D_TextBuf C2D_TextBufNew(size_t maxGlyphs)
{
    (void)maxGlyphs;
    text_storage.used = 0;
    return &text_storage;
}

void C2D_TextBufDelete(C2D_TextBuf buf)
{
    (void)buf;
}

void C2D_TextBufClear(C2D_TextBuf buf)
{
    buf->used = 0;
}

const char *C2D_TextParse(C2D_Text *text, C2D_TextBuf buf, const char *str)
{
    const size_t length = strlen(str);
    if (buf->used + length + 1 > ARENA_CAPACITY) {
        return NULL;
    }
    memcpy(buf->arena + buf->used, str, length + 1);
    text->buf = buf;
    text->begin = buf->used;
    text->end = length;
    text->lines = 1;
    text->words = 1;
    text->font = NULL;
    buf->used += length + 1;
    return str + length;
}

void C2D_TextOptimize(const C2D_Text *text)
{
    (void)text;
}

static float measure_native(const char *text)
{
    float width = 0.0f;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0';
         cursor++) {
        if (*cursor >= 0x20 && *cursor < 0x80) {
            width += (float)advance_table[*cursor - 0x20];
        } else if (*cursor >= 0x80) {
            width += (float)advance_table['n' - 0x20];
        }
    }
    return width;
}

void C2D_DrawText(
    const C2D_Text *text,
    u32 flags,
    float x,
    float y,
    float z,
    float scaleX,
    float scaleY,
    ...
)
{
    (void)z;
    (void)scaleX;

    u32 color = 0xFFFFFFFFu;
    if ((flags & C2D_WithColor) != 0) {
        va_list arguments;
        /*
         * citro2d's own prototype ends in a float, so the promotion warning is
         * inherent to matching its signature rather than a defect here.
         */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvarargs"
        va_start(arguments, scaleY);
#pragma GCC diagnostic pop
        color = va_arg(arguments, u32);
        va_end(arguments);
    }

    const char *source = text->buf->arena + text->begin;
    const char *anchor = "start";
    if ((flags & C2D_AlignMask) == C2D_AlignRight) {
        anchor = "end";
    } else if ((flags & C2D_AlignMask) == C2D_AlignCenter) {
        anchor = "middle";
    }

    char escaped[1024];
    size_t written = 0;
    for (const char *cursor = source;
         *cursor != '\0' && written + 8 < sizeof(escaped);
         cursor++) {
        if (*cursor == '&') {
            memcpy(escaped + written, "&amp;", 5);
            written += 5;
        } else if (*cursor == '<') {
            memcpy(escaped + written, "&lt;", 4);
            written += 4;
        } else if (*cursor == '>') {
            memcpy(escaped + written, "&gt;", 4);
            written += 4;
        } else if (*cursor == '\n' || *cursor == '\r') {
            escaped[written++] = ' ';
        } else {
            escaped[written++] = *cursor;
        }
    }
    escaped[written] = '\0';

    unsigned int red, green, blue;
    float opacity;
    color_parts(color, &red, &green, &blue, &opacity);

    const float measured = measure_native(source) * scaleY;
    buffer_append(
        &screens[active_screen],
        "    <text x=\"%.2f\" y=\"%.2f\" text-anchor=\"%s\" xml:space=\"preserve\""
        " font-family=\"'Avenir Next', 'Segoe UI', system-ui, sans-serif\""
        " font-size=\"%.2f\" textLength=\"%.2f\""
        " lengthAdjust=\"spacingAndGlyphs\""
        " fill=\"rgb(%u,%u,%u)\" fill-opacity=\"%.3f\">%s</text>\n",
        x, y + 25.0f * scaleY, anchor,
        30.0f * scaleY, measured,
        red, green, blue, opacity, escaped
    );
}

/* ------------------------------------------------------------------- font -- */

int C2D_FontGlyphIndexFromCodePoint(C2D_Font font, u32 codepoint)
{
    (void)font;
    return (int)codepoint;
}

charWidthInfo_s *C2D_FontGetCharWidthInfo(C2D_Font font, int glyphIndex)
{
    (void)font;
    width_info.left = 0;
    if (glyphIndex >= 0x20 && glyphIndex < 0x80) {
        width_info.charWidth = advance_table[glyphIndex - 0x20];
    } else {
        width_info.charWidth = advance_table['n' - 0x20];
    }
    width_info.glyphWidth = width_info.charWidth;
    return &width_info;
}

void C2D_FontCalcGlyphPos(
    C2D_Font font,
    fontGlyphPos_s *out,
    int glyphIndex,
    u32 flags,
    float scaleX,
    float scaleY
)
{
    (void)font;
    (void)glyphIndex;
    (void)flags;
    (void)scaleX;
    (void)scaleY;
    /* Matches the real libctru convention: sampled v runs bottom-up. */
    out->texcoord.left = 0.0f;
    out->texcoord.top = 1.0f;
    out->texcoord.right = 0.05f;
    out->texcoord.bottom = 0.9f;
}

/* ---------------------------------------------------------------- utility -- */

ssize_t decode_utf8(uint32_t *out, const uint8_t *in)
{
    if (in[0] < 0x80) {
        *out = in[0];
        return in[0] == 0 ? 0 : 1;
    }
    if ((in[0] & 0xE0) == 0xC0 && (in[1] & 0xC0) == 0x80) {
        *out = (uint32_t)((in[0] & 0x1F) << 6) | (in[1] & 0x3F);
        return 2;
    }
    if ((in[0] & 0xF0) == 0xE0 && (in[1] & 0xC0) == 0x80
        && (in[2] & 0xC0) == 0x80) {
        *out = (uint32_t)((in[0] & 0x0F) << 12)
            | (uint32_t)((in[1] & 0x3F) << 6)
            | (in[2] & 0x3F);
        return 3;
    }
    return -1;
}

const char *preview_screen_svg(int screen)
{
    return screens[screen].data != NULL ? screens[screen].data : "";
}

const char *preview_defs_svg(void)
{
    return defs.data != NULL ? defs.data : "";
}
