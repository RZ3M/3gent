/*
 * Host stand-in for libctru's <3ds.h>, used only by tools/ui-preview.
 *
 * It exists so `client-3ds/source/ui.c` can be compiled unmodified on a laptop.
 * Only the declarations that file actually touches are provided.
 */
#ifndef THREEGENT_PREVIEW_3DS_H
#define THREEGENT_PREVIEW_3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

typedef struct {
    s8 left;
    u8 glyphWidth;
    u8 charWidth;
} charWidthInfo_s;

typedef struct {
    int sheetIndex;
    float xOffset;
    float xAdvance;
    float width;
    struct {
        float left, top, right, bottom;
    } texcoord;
    struct {
        float left, top, right, bottom;
    } vtxcoord;
} fontGlyphPos_s;

typedef enum {
    GFX_TOP = 0,
    GFX_BOTTOM = 1,
} gfxScreen_t;

typedef enum {
    GFX_LEFT = 0,
    GFX_RIGHT = 1,
} gfx3dSide_t;

void gfxInitDefault(void);
void gfxExit(void);
u64 osGetTime(void);
ssize_t decode_utf8(uint32_t *out, const uint8_t *in);

#endif
