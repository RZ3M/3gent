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
the microphone halo, the spinner, and the face-button key caps.

Tinted surfaces are pre-mixed to an opaque colour (`ui_blend`) rather than drawn
translucent, because an outlined panel puts a filled border underneath the fill.

The large panels — the approval card, the paired-machine card, the task detail
card, the decision hero — all go through `ui_card`, which carries the accent in
three places at once: an opaque border mixed toward it, a fill tinted a few
percent toward it, and a tick **inset inside** the surface. The earlier version
put a 3 px bar on the panel's own left edge, and on hardware that reads as a
stroke floating beside the panel rather than part of it: the border it sits
against is a single dim pixel, and the chamfer cuts the bar short of both
corners. The inset tick is the grammar the bottom-screen list rows already use,
which is the one that was already reading correctly on the panel.

### Key caps

A key cap is drawn as the key is actually shaped. `A`, `B`, `X`, `Y`, `START`
and `SELECT` are round on the hardware, so their caps are circles; only the
shoulders are rectangles. A square `A` sends the thumb to the wrong place. One
helper, `ui_key_cap`, owns the distinction, so the action bar and the
push-to-talk panel cannot disagree about what a key looks like.

### Motion

Animation is derived from `osGetTime()`, so it is frame-rate independent:

- an eight-dot spinner for indeterminate work;
- a sweeping block for an active turn;
- a pulsing ring while recording;
- a scrolling level trace built from a 28-slot history of microphone level, so
  the meter reads as motion even when the level is steady.

Frame-rate independence is not the same as getting frames. The synchronous
network calls — warming the connections, resuming a session — used to block the
one thread that draws, so the spinner they were put on screen to justify sat on
a single frame for the whole wait. `network_set_wait_callback` hands the network
module a redraw to call between wait slices, so anything animated behind a
blocking request keeps moving. Nothing that animates may depend on the loop that
would have been blocked.

## 4. Navigation model

There is one stack, and one way out of each level:

```text
start screen  ──A──►  tasks  ──A──►  task
     ▲                   ▲             │
     └───────B───────────┘◄─────B──────┘
   START exits                       START jumps to the start screen
```

`A` accepts and `B` goes back, on every screen and without exception. `B` is
consumed first by anything that is a decision — declining an approval,
cancelling a transcript — and only then means "back", so a decision can never be
skipped by reflex.

`START` is the only key that is not part of the stack. It exits the application
from the start screen and returns to the start screen from anywhere else. It is
named on screen only where it is the intended action, which is the start screen.

## 5. Screen roles

### Start screen

The application opens on the start screen, not on a connection attempt. The top
screen states the one fact that decides everything else — whether a machine is
paired, and which — and the bottom screen is a five-row menu: connect, pair by
QR, pair by typed code, forget this machine, exit.

Every row carries its own one-line explanation and is itself the touch target,
so this screen needs no action bar — only the hint line that names `START`, the
one place where exiting is the intended action.

"Forget this machine" is dimmed rather than hidden when nothing is paired. It is
the one deliberate exception to hiding impossible actions: a destructive action
that vanishes when unavailable is a destructive action the user cannot learn the
position of.

### Pairing screen

Full-bleed viewfinder with corner brackets rather than a full frame: the
brackets mark the target without covering the code the user is trying to fill
them with. A counter shows how many frames have actually been examined, so a
decoder that is working but not yet succeeding does not look like a frozen one.

Decoding, exchanging, success and failure each replace the viewfinder with the
same attention card the approval and transcript flows use. Failure is always
recoverable in place: rescan, type the code by hand, or go back.

### Task manager

The list is on the **bottom** screen, because that is the screen a finger can
reach, and the top screen carries the detail of whichever task is highlighted:
its state in words, why it is blocked, and whether it is the one already open.
Two screens, two jobs.

A summary line states what the user would otherwise have to count — `6 TASKS`
and `2 waiting on you`. "Start a new task" is the last row rather than a
separate button, so starting a task is reached the same way as opening one: the
D-pad and Circle Pad walk onto it past the end of the list, `A` takes it, and
the top screen describes it like any other row. `X` remains the shortcut from
anywhere in the list. The row is absent while the bridge is unreachable, because
starting a task would fail exactly as listing them just did.

The state pill on this screen reports the **bridge**, not the agent. There is no
open task to have a state, and the last one's state goes stale the moment the
user leaves it — which is how the pill came to read `CONNECTING` over a list
that could only have arrived from a machine that answered. It now reads
`LOADING`, `CONNECTED` or `OFFLINE`, all three of which the screen can prove.

### Top screen — read surface

400×240, three bands:

- **header (30 px):** gradient accent rule, `3gent` wordmark, the task position
  (`2/6`), the active task label, and the agent-state pill on the right. The
  position is what makes switching legible: without it a changed label gives no
  clue whether the user moved one step or wrapped around;
- **body:** an optional one-line prompt echo with an azure rule, then the
  wrapped response with a proportional scrollbar. Word wrapping is measured
  from real glyph advances and cached until the response changes;
- **footer (24 px):** diff summary (`2 files  +31  -12`), scrollback position,
  and on the right whichever of two facts deserves the next glance — `2 other
  tasks need you` in coral, or the applied event cursor when nothing does.

The response buffer keeps the **newest** output. When it fills, whole lines are
dropped from the front rather than discarding what just arrived; a monitoring
screen that silently stops updating is worse than one that forgets history.

Two situations take the whole read surface:

- **approval required** — a coral card over a dimmed body;
- **transcript review** — a mint card with the transcript to send;
- **recording** — a pulsing ring, `LISTENING`, and the elapsed time, because
  while the microphone is open there is nothing else to read.

### Bottom screen — action surface

320×240, five bands:

- **task rail (32 px):** up to four tasks as tabs, each with a state dot, plus a
  manager button carrying a coral badge when something elsewhere wants the user.
  A task that is blocked also pulses, so "needs you" is not carried by hue
  alone. Tapping a tab switches to it;
- **status (46 px):** the current phase plus two diagnostic detail lines, and —
  only when the response is longer than the screen — a three-button scroll
  cluster: page back, page forward, jump to newest;
- **hero (72 px):** the one thing to do now — hold-to-talk, agent working, or
  what is actually being decided. Recording takes the action band as well,
  because there is no competing action while the microphone is open;
- **action bar (52 px):** the actions that are live right now, as touch targets
  with their key caps, and nothing else. Two across sit the cap beside the
  label; three or four stack the cap above it, because a cap beside a 70 px
  button leaves under 30 px for the word and the device font — wider than the
  preview's estimate — turns `Photo` into `Ph...`. The button rectangle, and so
  the touch target, is the same either way;
- **status bar (36 px):** link state, bridge address, and microphone/audio/photo
  tokens.

## 6. Physical-button mapping

| Key | Task | Task manager | Start screen | Pairing | Photo |
| --- | --- | --- | --- | --- | --- |
| `A` | approve · send transcript · type | open · start the new-task row | select | rescan/continue | attach |
| `B` | decline · cancel transcript · **back to tasks** | back | — | cancel | discard |
| `X` | interrupt while working, otherwise new task | new task | — | — | — |
| `Y` | edit a pending transcript | — | — | type the code | — |
| `L` | photo for the next prompt | — | — | — | — |
| hold `R` | push-to-talk | — | — | — | — |
| ↑ ↓ | scroll, with held repeat | choose | choose | — | — |
| ← → | previous / next task | — | — | — | — |
| `START` | start screen | start screen | **exit** | start screen | — |

The D-pad and Circle Pad are interchangeable everywhere. Up and down always mean
"move through what you are reading"; left and right always mean "move between
tasks". In the task manager, up and down reach every row on screen including
"start a new task" — a row a stylus could take but the keys could not was a row
in two different lists.

### Actions are shown only when they exist

There is no permanent grid of key hints. Each screen answers "what can I do
right now" and the action bar draws exactly that: two buttons for an approval,
three for a transcript, four when idle, none while recording. An action that is
impossible is absent rather than dimmed — a standing row of dead keys teaches
the user to stop reading it.

Screens whose content is a list are the exception: their rows are the targets,
so they carry one quiet line of key hints instead of a second bank of buttons
competing with the list.

## 7. Touch

The bottom screen is a real input surface, not a second display.

`ui_hit_test` resolves a point to the same semantic action the renderer drew
there, sharing the renderer's layout functions, so a target cannot drift away
from the thing that looks like it. `main.c` folds the result back into the key
that already implements the action, which means there is exactly one
implementation of every action and touch cannot develop its own behaviour.

Targets are **armed on contact and fire on release over the same target**, so a
mis-aimed stylus is recoverable by sliding off before lifting. The touch
position is only meaningful while contact is held — after the lift the hardware
reports the origin, which is a live target — so the last held point is retained
and the release is judged against that.

Reachable by stylus: task tabs, the manager button, list rows, every action-bar
button, the scroll cluster, and the push-to-talk panel, which is held rather
than tapped and is the one-handed alternative to holding `R`.

## 8. Voice UX

1. Hold `R`, or hold the push-to-talk panel with the stylus.
2. Both screens switch to the recording treatment: elapsed time, level trace,
   and progress toward the five-minute bound.
3. Release.
4. Upload and transcription state.
5. The transcript appears in a review card.
6. Explicitly send (`A`), edit (`Y`), or cancel (`B`).

Review-before-send is the accepted default; voice capture never silently becomes
an agent prompt immediately after release.

## 9. Text UX

Use the native software keyboard. It is a modal applet, so the pushed control
link is stopped and restarted around it.

## 10. Approval UX

The top card shows what wants permission and the command or path it affects,
wrapped over up to five lines with an explicit overflow count. The bottom screen
repeats the first three lines above two large buttons carrying their key caps,
so the decision and the thing being decided are on the same screen as the finger.

`A` approves and `B` declines, which is the same grammar as everywhere else. The
product rule that approvals must be hard to trigger by accident is preserved by
arming instead of by placement: `A` is ignored for 450 ms after the request
appears, so a press already travelling toward something else cannot answer it,
and the screen says "Read it first, then approve" if one does. See D-023.

## 11. Camera and QR

Implemented pairing: the bridge prints a QR code (and an SVG for a brighter,
larger target), the handheld opens a viewfinder, and the outcome is immediate
and named. Decoding runs on a worker thread so the viewfinder never stalls. The
typed fallback is one keyboard prompt taking the four values the bridge printed,
and it also accepts a pasted `3gent://pair?...` URL, so a user who reaches for
the wrong entry point still succeeds.

Implemented photo capture: shoot, review full-screen with a caption scrim,
attach with `A` or discard with `B`, then a bounded upload with a progress bar.
The RGB565 frame is tiled into a GPU texture; the sampled v-axis convention is
detected at runtime from the system font rather than assumed.

## 12. Stylus capture (outside the current goal)

If revisited later: full bottom-screen canvas, clear, undo, pen thickness, send.

## 13. Accessibility/reliability principles

- every network action has visible status;
- every failure has a retry path;
- QR has manual fallback;
- voice has text fallback;
- do not rely only on colour to show agent state;
- avoid tiny interaction targets.

## 14. Reviewing the interface without hardware

`tools/ui-preview/` compiles `client-3ds/source/ui.c` against a recording
backend and writes one SVG per interface state plus an `index.html` contact
sheet:

```sh
cd tools/ui-preview
make run
```

The geometry is the shipping geometry. Glyph advances are approximated off
device, so treat text fit as indicative and confirm it on hardware.
