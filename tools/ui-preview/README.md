# Handheld interface preview

Renders every 3gent interface state to SVG on a laptop, so the handheld layout
can be reviewed without flashing a build.

It compiles the real `client-3ds/source/ui.c`. The shim headers in `shim/`
shadow `<3ds.h>` and `<citro2d.h>` on the host include path, and `svg_backend.c`
records the draw calls instead of rasterising them. The renderer file itself
needs no host-specific code, so the preview cannot drift from the device.

## Run it

```sh
cd tools/ui-preview
make run
```

Output lands in `out/`: one SVG per state plus `index.html`, a contact sheet.
Open `out/index.html` in a browser.

## What it does and does not prove

Proves:

- panel, card, chip, meter and scrollbar geometry;
- band heights and whether anything overlaps or overflows a surface;
- colour composition, including tinted panels over borders;
- context-sensitive states — which chips are live, which hero is shown;
- animated frames, because `preview_set_time()` pins `osGetTime()`.

Does not prove:

- **text fit.** The 3DS shared system font is not available off device, so glyph
  advances come from the approximation table in `svg_backend.c`. Each text box is
  emitted with an explicit `textLength`, so the preview reproduces the width
  `ui.c` computed rather than the browser's — but that width is itself an
  estimate. Confirm legibility and truncation on hardware.
- **the camera preview.** There is no photo on the host, so `C2D_DrawImageAt`
  draws a labelled placeholder.
- anything about frame rate, battery, input latency or GPU memory.

## Adding a state

Add a fixture to `preview.c`: fill a `UiModel`, call `preview_set_time()`,
`ui_render()`, then `write_document()`. States with motion can be rendered over
several frames; only the last one is written, which is how the recording level
trace gets a filled history.
