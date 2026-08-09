/*
 * Host stand-in for <citro2d.h>/<citro3d.h>, used only by tools/ui-preview.
 *
 * Every entry point below is implemented by svg_backend.c, which records the
 * draw calls instead of rasterising them. Only the surface that
 * `client-3ds/source/ui.c` uses is declared.
 */
#ifndef THREEGENT_PREVIEW_CITRO2D_H
#define THREEGENT_PREVIEW_CITRO2D_H

#include <3ds.h>

#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
#define C2D_DEFAULT_MAX_OBJECTS 4096
#define C3D_FRAME_SYNCDRAW 0x01

typedef enum {
    GPU_RGB565 = 3,
} GPU_TEXCOLOR;

typedef enum {
    GPU_NEAREST = 0,
    GPU_LINEAR = 1,
} GPU_TEXTURE_FILTER_PARAM;

typedef struct {
    void *data;
    u16 width;
    u16 height;
    GPU_TEXCOLOR format;
} C3D_Tex;

typedef struct {
    u16 width;
    u16 height;
    float left, top, right, bottom;
} Tex3DS_SubTexture;

typedef struct {
    C3D_Tex *tex;
    const Tex3DS_SubTexture *subtex;
} C2D_Image;

typedef struct C3D_RenderTarget_tag C3D_RenderTarget;
typedef struct C2D_TextBuf_s *C2D_TextBuf;
typedef struct C2D_Font_s *C2D_Font;

typedef struct {
    C2D_TextBuf buf;
    size_t begin;
    size_t end;
    float width;
    u32 lines;
    u32 words;
    C2D_Font font;
} C2D_Text;

enum {
    C2D_AtBaseline = 1 << 0,
    C2D_WithColor = 1 << 1,
    C2D_AlignLeft = 0 << 2,
    C2D_AlignRight = 1 << 2,
    C2D_AlignCenter = 2 << 2,
    C2D_AlignJustified = 3 << 2,
    C2D_AlignMask = 3 << 2,
    C2D_WordWrap = 1 << 4,
};

static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a)
{
    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24);
}

bool C3D_Init(size_t commandBufferSize);
void C3D_Fini(void);
bool C3D_FrameBegin(u8 flags);
void C3D_FrameEnd(u32 flags);

bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format);
void C3D_TexSetFilter(
    C3D_Tex *tex,
    GPU_TEXTURE_FILTER_PARAM magFilter,
    GPU_TEXTURE_FILTER_PARAM minFilter
);
void C3D_TexFlush(C3D_Tex *tex);
void C3D_TexDelete(C3D_Tex *tex);

bool C2D_Init(size_t maxObjects);
void C2D_Fini(void);
void C2D_Prepare(void);
C3D_RenderTarget *C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side);
void C2D_TargetClear(C3D_RenderTarget *target, u32 color);
void C2D_SceneBegin(C3D_RenderTarget *target);

bool C2D_DrawRectangle(
    float x, float y, float z, float w, float h,
    u32 clr0, u32 clr1, u32 clr2, u32 clr3
);
bool C2D_DrawTriangle(
    float x0, float y0, u32 clr0,
    float x1, float y1, u32 clr1,
    float x2, float y2, u32 clr2,
    float depth
);
bool C2D_DrawEllipse(
    float x, float y, float z, float w, float h,
    u32 clr0, u32 clr1, u32 clr2, u32 clr3
);
bool C2D_DrawImage(C2D_Image img, const void *params, const void *tint);

static inline bool C2D_DrawRectSolid(
    float x, float y, float z, float w, float h, u32 clr
)
{
    return C2D_DrawRectangle(x, y, z, w, h, clr, clr, clr, clr);
}

static inline bool C2D_DrawCircleSolid(
    float x, float y, float z, float radius, u32 clr
)
{
    return C2D_DrawEllipse(
        x - radius, y - radius, z, radius * 2.0f, radius * 2.0f,
        clr, clr, clr, clr
    );
}

bool C2D_DrawImageAt(
    C2D_Image img,
    float x,
    float y,
    float depth,
    const void *tint,
    float scaleX,
    float scaleY
);

C2D_TextBuf C2D_TextBufNew(size_t maxGlyphs);
void C2D_TextBufDelete(C2D_TextBuf buf);
void C2D_TextBufClear(C2D_TextBuf buf);
const char *C2D_TextParse(C2D_Text *text, C2D_TextBuf buf, const char *str);
void C2D_TextOptimize(const C2D_Text *text);
void C2D_DrawText(
    const C2D_Text *text,
    u32 flags,
    float x,
    float y,
    float z,
    float scaleX,
    float scaleY,
    ...
);

int C2D_FontGlyphIndexFromCodePoint(C2D_Font font, u32 codepoint);
charWidthInfo_s *C2D_FontGetCharWidthInfo(C2D_Font font, int glyphIndex);
void C2D_FontCalcGlyphPos(
    C2D_Font font,
    fontGlyphPos_s *out,
    int glyphIndex,
    u32 flags,
    float scaleX,
    float scaleY
);

#endif
