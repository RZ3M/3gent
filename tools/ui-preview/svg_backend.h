#ifndef THREEGENT_PREVIEW_SVG_BACKEND_H
#define THREEGENT_PREVIEW_SVG_BACKEND_H

#include <3ds.h>

/* Pins osGetTime() so animated states render deterministically. */
void preview_set_time(u64 milliseconds);

/* Draw output from the most recent frame. 0 is the top screen, 1 the bottom. */
const char *preview_screen_svg(int screen);
const char *preview_defs_svg(void);

#endif
