# 3gent — UI/UX Direction

**Status:** Implemented for the current hardware-test build

## 1. Device philosophy

The 3DS should feel like a handheld control surface, not a squeezed desktop UI.

Use physical controls aggressively.

## 2. Rendering

The interface is drawn with **citro2d** on the PICA200, not with libctru's
`PrintConsole`. See D-020. Practical consequences:

- both screens are redrawn from scratch every frame, so there is no partial
  console repaint and no framebuffer flicker to manage;
- the shared system font gives proportional text at arbitrary scales rather than
  a fixed 8×8 grid;
- state can be shown with colour, weight, position and motion instead of only
  with words.

`client-3ds/source/ui.c` owns every pixel. It never touches the network,
microphone or camera: `main.c` fills a `UiModel` each frame and calls
`ui_render`. Keeping the renderer free of side effects is what lets
`tools/ui-preview/` compile the same file on a laptop.

## 3. Visual system

### Palette

| Token | Value | Use |
| --- | --- | --- |
| ink / ink-dim / ink-faint | `#E9EDF7` `#93A0BA` `#5D6883` | text hierarchy |
| bg0 / bg1 / bg2 | `#0B0E15` `#10141E` `#181D2B` | page, chrome, raised surface |
| line | `#252D40` | hairlines and borders |
| mint `#4FE0A6` | ready, healthy, send |
| amber `#FFC15A` | agent working |
| coral `#FF8A62` | approval required |
| rose `#FF5C79` | recording, failure |
| azure `#74A8FF` | selection, links, photos |
| violet `#A98BFF` | brand accent only |

Agent state is never carried by colour alone. Every state also has a word
(`READY`, `WORKING`, `APPROVAL`) and, where it is live, motion.

### Form

Panels are **chamfered**, not rounded. A 3 px bevel reads as deliberate at this
pixel density and avoids citro2d's expensive circle-mode state change on every
surface. Circles are reserved for things that are genuinely round: status dots,
the microphone halo, and the spinner.

Tinted surfaces are pre-mixed to an opaque colour (`ui_blend`) rather than drawn
translucent, because an outlined panel puts a filled border underneath the fill.

### Motion

Animation is derived from `osGetTime()`, so it is frame-rate independent:

- an eight-dot spinner for indeterminate work;
- a sweeping block for an active turn;
- a pulsing ring while recording;
- a scrolling level trace built from a 28-slot history of microphone level, so
  the meter reads as motion even when the level is steady.

## 4. Screen roles

### Top screen — read surface

400×240, three bands:

- **header (30 px):** gradient accent rule, `3gent` wordmark, active task label,
  and the agent-state pill on the right;
- **body:** an optional one-line prompt echo with an azure rule, then the
  wrapped response with a proportional scrollbar. Word wrapping is measured
  from real glyph advances and cached until the response changes;
- **footer (24 px):** diff summary (`2 files  +31  -12`), scrollback position,
  and the applied event cursor.

Two situations take the whole read surface:

- **approval required** — a coral card over a dimmed body;
- **transcript review** — a mint card with the transcript to send;
- **recording** — a pulsing ring, `LISTENING`, and the elapsed time, because
  while the microphone is open there is nothing else to read.

### Bottom screen — action surface

320×240, four bands:

- **status (52 px):** the current phase plus two diagnostic detail lines;
- **hero (96 px):** the one thing to do now — hold-to-talk, live recording
  meter, agent working, transcript decision, or the approval choice;
- **chips (56 px):** six context-aware button hints; unavailable actions are
  dimmed rather than hidden, so the mapping stays learnable;
- **status bar (36 px):** link state, bridge address, and microphone/audio/photo
  tokens.

## 5. Physical-button mapping

Current build:

- `A`: type and send, or send a reviewed transcript;
- `X`: approve once when an approval is pending, otherwise send the
  approval-demo prompt;
- `B`: decline, interrupt, or cancel a transcript, in that order of precedence;
- `Y`: edit a pending transcript in the native keyboard;
- hold `R`: push-to-talk;
- `L`: capture a photo for the next prompt;
- D-pad/Circle Pad Up/Down: scroll, including held repeat;
- `START`: exit.

Approval ergonomics are still pending usability testing. The approve action is
deliberately not on `A`, and it is drawn as a distinct two-button choice rather
than a single reflexive confirm.

## 6. Voice UX

1. Hold push-to-talk.
2. Both screens switch to the recording treatment: elapsed time, level trace,
   and progress toward the five-minute bound.
3. Release.
4. Upload and transcription state.
5. The transcript appears in a review card.
6. Explicitly send (`A`), edit (`Y`), or cancel (`B`).

Review-before-send is the accepted default; voice capture never silently becomes
an agent prompt immediately after release.

## 7. Text UX

Use the native software keyboard. It is a modal applet, so the pushed control
link is stopped and restarted around it.

## 8. Approval UX

The card shows what wants permission and the command or path it affects, wrapped
over up to five lines with an explicit overflow count. The bottom screen shows
the two choices as separate buttons with their key caps.

## 9. Camera and QR

Pairing (future): desktop shows QR, camera view opens, immediate success/failure,
manual fallback remains available.

Implemented photo capture: shoot, review full-screen with a caption scrim,
attach with `A` or discard with `B`, then a bounded upload with a progress bar.
The RGB565 frame is tiled into a GPU texture; the sampled v-axis convention is
detected at runtime from the system font rather than assumed.

## 10. Stylus capture (outside the current goal)

If revisited later: full bottom-screen canvas, clear, undo, pen thickness, send.

## 11. Accessibility/reliability principles

- every network action has visible status;
- every failure has a retry path;
- QR has manual fallback;
- voice has text fallback;
- do not rely only on colour to show agent state;
- avoid tiny interaction targets.

## 12. Reviewing the interface without hardware

`tools/ui-preview/` compiles `client-3ds/source/ui.c` against a recording
backend and writes one SVG per interface state plus an `index.html` contact
sheet:

```sh
cd tools/ui-preview
make run
```

The geometry is the shipping geometry. Glyph advances are approximated off
device, so treat text fit as indicative and confirm it on hardware.
