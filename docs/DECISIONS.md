# 3gent — Decision Log

Use this file as the index. Major decisions can get full ADRs in `docs/adr/`.

## Accepted

### D-001 — Open source

**Status:** Accepted

3gent is intended as a public open-source project that others can use and contribute to.

### D-002 — Developer audience

**Status:** Accepted

Target users are developers with modded 3DS-family systems who already understand coding-agent workflows.

### D-003 — Agent-agnostic core

**Status:** Accepted

Agent-specific integrations belong behind desktop-side adapters.

### D-004 — Thin 3DS client

**Status:** Accepted

The 3DS handles capture, display, navigation, and remote control. Heavy processing and repository access remain off-device.

### D-005 — Remote access is core

**Status:** Accepted

3gent is not limited to the same LAN as the development computer.

### D-006 — Hotspot workflow is first-class

**Status:** Accepted

Because the 3DS has older 2.4 GHz Wi-Fi capabilities and public networks are unpredictable, a compatible phone/computer hotspot is a normal connection path.

### D-007 — Voice-first input

**Status:** Accepted

Push-to-talk voice is the primary input experience.

### D-008 — Native software keyboard

**Status:** Accepted

Typed prompts are an MVP fallback using the system software keyboard.

### D-009 — Unified Capture abstraction

**Status:** Accepted

Text, audio, photo, and sketch inputs share one product-level Capture concept.

### D-010 — QR pairing default

**Status:** Accepted

QR code is the default pairing UX. Manual pairing remains a fallback.

### D-011 — No full terminal emulator in MVP

**Status:** Accepted

3gent is agent-first. Optional raw-terminal functionality can be considered later.

### D-012 — Codex adapter should use structured interface

**Status:** Accepted in principle

Prefer Codex app-server through the desktop bridge over terminal scraping.

### D-013 — TypeScript/Node desktop bridge

**Status:** Accepted

The first production-shaped desktop bridge uses TypeScript on a supported Node.js
LTS release. It keeps Stage 1 iteration fast, fits the JSON/event-oriented
protocol, and provides a straightforward subprocess and JSON-RPC boundary for
agent adapters. Agent-specific objects still stop at the adapter boundary.

This does not require the relay or every future component to use TypeScript.

### D-014 — New and resumed agent sessions

**Status:** Accepted

The product supports both starting a new coding-agent session and resuming an
existing one. The first Codex adapter should expose both operations while the
desktop bridge remains authoritative over workspace and policy choices.

### D-015 — Review voice transcripts before sending

**Status:** Accepted

Releasing push-to-talk uploads and transcribes the capture, then shows the
transcript on the 3DS. A separate explicit button sends it to the agent. The
user can cancel and may edit through the native keyboard where practical.
Version `0.3.0-stage3` implements this behavior for both fake and real adapters.

### D-016 — Self-hosted relay first

**Status:** Accepted

The first remote relay is self-hosted. Its protocol must not prevent a later
hosted service, but an official public relay is not required for the first
functional product.

### D-017 — Photo included; stylus sketch deferred

**Status:** Accepted for the current product scope

Photo capture and attachment are included after the core agent loop. Stylus
sketch capture is outside the current goal and may be revisited later.

### D-018 — Codex app-server translation boundary

**Status:** Accepted

The initial Codex adapter spawns the installed `codex app-server` over stdio,
inspects the installed schema, and translates only supported v2 methods/events
at the desktop boundary. Codex thread UUIDs are replaced by deterministic opaque
3gent session IDs. Remote approvals expose only one-shot accept, decline and
cancel; session-wide policy amendments and extended permission profiles remain
desktop-only.

### D-019 — Reverse relay for the self-hosted hardware test

**Status:** Accepted as a reversible test only

The first remote feasibility build uses a small self-hosted reverse TCP relay so
the laptop opens outbound uplinks and the 3DS can reach both HTTP media/session
traffic and pushed control. Bridge uplinks use a bounded token and connection
pool. Per explicit current scope, the 3DS-facing side remains unauthenticated
plaintext and the relay requires `--unsafe-public`; this is not acceptance of a
production transport and must be replaced or secured before release.

### D-020 — citro2d for the handheld interface

**Status:** Accepted

The handheld interface is drawn with citro2d on the PICA200 instead of
libctru's `PrintConsole` text grid.

Alternatives considered:

- **Keep `PrintConsole`.** Smallest change and lowest memory, but the product is
  a state display: a fixed 8×8 monochrome grid cannot show agent state,
  approvals, recording level or diff summaries as anything other than more
  lines of text. It also caused R-020's flicker, because correctness depended on
  tracking which framebuffer had been partially repainted.
- **Direct framebuffer drawing.** Full control with no new dependency, but it
  means hand-writing blitting, alpha blending, and above all text layout against
  the shared system font. That is the bulk of citro2d.
- **citro2d.** Ships with the devkitPro `3ds-dev` group already required by this
  project, is the maintained upstream 2D layer, renders the shared system font
  at arbitrary scale, and runs on Old 3DS.

Consequences:

- both screens are fully redrawn each frame, which removes the partial-repaint
  class of bug entirely rather than managing it;
- the camera preview can no longer be written straight into the framebuffer and
  is uploaded as a tiled GPU texture instead (see R-021);
- rendering is isolated in `client-3ds/source/ui.c` behind a `UiModel` struct, so
  it stays testable off device and cannot acquire side effects;
- `libcitro2d` and `libcitro3d` join the link line.

This does not change the thin-client invariant. The 3DS still renders state that
the bridge owns.

### D-023 — Handheld navigation grammar, touch, and live task switching

**Status:** Accepted

The interface now has one navigation grammar: `A` accepts, `B` goes back, the
bottom screen is touchable, and tasks are switched without leaving the
conversation.

This reverses a previously documented choice. `UI_UX.md` said the approve action
was deliberately not on `A`. Keeping it off `A` meant `A` had no stable meaning
(it was "type", "send", or nothing), `B` meant three unrelated things depending
on state, and `START` — the key with the strongest "quit" connotation on the
hardware — was the only way back. The cost of that was paid on every screen to
protect one.

Alternatives considered:

- **Keep approve on `X`.** Preserves the existing muscle memory and needs no
  mitigation. But it leaves the whole application without a consistent accept
  key, which is a larger and more frequent harm than the one it avoids.
- **Approve on `A`, unmitigated.** Clean grammar, but it violates the standing
  product rule that approvals must be hard to trigger accidentally (PRODUCT.md
  §3.6), because the same key sends prompts and dismisses cards.
- **Approve on `A`, armed.** `A` is ignored for 450 ms after the request
  appears. A press already in flight toward something else cannot answer it, and
  a deliberate press a moment later works normally. The two choices are still
  drawn as separate labelled buttons, never a single reflexive confirm.

Decision: the third. The invariant is preserved by timing rather than by
placement, and the button grammar becomes learnable everywhere else.

Also accepted as part of the same change:

- **`B` returns to the task manager, not the start screen.** Navigation is a
  stack: start screen → tasks → task. `START` remains the escape hatch and is
  named on screen only where exiting is the intended action.
- **The bottom screen is an input surface.** `ui_hit_test` resolves a point to
  the action the renderer drew there, sharing the renderer's layout so a target
  cannot drift from its appearance, and `main.c` folds a hit into the key that
  already implements it. Touch therefore cannot acquire behaviour of its own.
- **Key hints are shown only when the action exists.** The fixed six-chip grid
  is replaced by an action bar built from the current model. Impossible actions
  are absent rather than dimmed; the one exception is destructive actions on
  list screens, which stay visible and dimmed so their position is learnable.
- **Tasks are switched in place.** The rail on the bottom screen and left/right
  on the D-pad or Circle Pad resume another task and repoint the pushed link
  without leaving the agent loop. Per-task state comes from `/v1/sessions`,
  which already reports `state`, `pendingApprovalId` and `lastSequence`, so
  attention and unread markers need no bridge change.
- **The response buffer keeps its tail.** Replaying a long task from cursor zero
  used to fill the buffer and then discard everything after it. It now drops
  whole lines from the front, which keeps memory bounded and keeps the newest
  output on screen.

Consequences: `UiModel` gains a task array, an active-task index and a touch
point; `ui.h` gains `ui_hit_test` and `ui_page_lines`; the agent loop is split
so the task manager and the conversation alternate. Ergonomics of the arming
delay and of four-across touch targets still need a physical pass — see R-022.

---

## Proposed

### D-021 — Vendor quirc as the handheld QR decoder

**Status:** Proposed

QR pairing (D-010) needs a decoder on the 3DS. devkitPro's `portlibs` does not
package one, so the choice is what to depend on and how.

Alternatives considered:

- **Write a decoder.** A QR reader is not a parser; it is perspective detection,
  binarisation, grid sampling and Reed-Solomon correction. Getting it wrong
  fails silently on a camera we cannot test in CI, and the project already
  refuses to hand-roll comparable primitives.
- **ZBar or zxing-cpp.** Both are maintained and capable. ZBar is larger and
  LGPL, which interacts with an unresolved licence question (D-P06); zxing-cpp
  is C++ and heavier than this client's dependency budget.
- **quirc 1.2, vendored unmodified.** ~3,000 lines of ISC C written for exactly
  this case: grayscale frames from an embedded camera. It bounds its own
  allocations, uses an explicit flood-fill stack rather than deep recursion, and
  is the decoder other 3DS homebrew already uses.

Decision: vendor quirc 1.2 into `client-3ds/third_party/quirc/` with no local
modifications, so re-importing upstream is a six-file copy.

Consequences:

- it does not build under this project's `-Wall -Wextra -Werror`, so those four
  objects are compiled with a relaxed warning set rather than patched; project
  code stays strict;
- roughly 120 KB of decoder working memory, allocated only while the pairing
  viewfinder is open;
- a full-frame decode costs far more than one frame's budget on Old 3DS, so it
  runs on a lower-priority worker thread (see R-006);
- ISC is permissive and compatible with every licence still under consideration
  for D-P06, but the vendored `LICENSE` must be preserved in any distribution.

Proposed rather than Accepted because the on-hardware scan-reliability evidence
that would justify locking in a decoder does not exist yet.

### D-022 — Device credential format and when it is enforced

**Status:** Proposed

Pairing issues a device token: 32 random bytes, base64url, presented as
`Authorization: Bearer` over HTTP and as `deviceToken` in the pushed-control
hello. The bridge persists only its SHA-256 hash plus a `dev_` identifier, a
name and timestamps, so a leaked `devices.json` yields no usable credential.
The handheld stores the token in cleartext at `sdmc:/3ds/3gent/pairing.cfg`,
which an SD card reader can read; that is what makes per-device revocation a
requirement rather than a nicety.

The open question is **enforcement**, not format. `--require-pairing` is
implemented and off by default. Turning it on by default now would mean sending
a bearer token over an unauthenticated plaintext link, where anyone able to
reach the port can also read the token in transit. That is not a security
boundary — it is an identity that looks like one, which is worse than none.

Enforcement should become the default in the same change that lands secure
transport (D-P11), not before. Until then the bridge accepts, records and can
verify credentials, and operators who want the check can ask for it.

### D-P02 — Old 3DS support

**Status:** Proposed default

Target Old 3DS compatibility unless measured performance makes a feature unreasonable.

Optimize for New 3DS only where explicitly documented.

### D-P06 — Project license

**Question:**
Choose a license before public release.

Candidates can be evaluated separately; do not insert a LICENSE file silently.

### D-P07 — Bridge ownership of agent lifetime

**Question:**
Should the bridge launch agents itself, integrate with a multiplexer such as Herdr, attach to existing agent sessions, or support all three via adapters?

### D-P08 — Voice capture transport

**Status:** Proposed / Stage 0 experiment

Candidate A: buffer a complete bounded capture on the 3DS, then upload.
- tolerates a temporarily unavailable connection after recording;
- simple file-oriented retry;
- recording duration consumes scarce handheld memory or SD-card I/O.

Candidate B: stream PCM while push-to-talk is held, then finalize media on the
desktop.
- constant and small 3DS memory use;
- removes handheld RAM as the practical duration limit;
- exposes connection stalls during capture and requires clean partial-stream
  handling.

Stage 0 uses Candidate B over a disposable HTTP chunked endpoint. Keep the
production decision Proposed until physical 3DS, hotspot, reconnect, and remote
transport behavior are measured.

### D-P09 — Persistent client transport

**Status:** Proposed / Stage 0 experiment

Candidate A: open a connection for each action.
- simplest failure isolation;
- physical testing measured user-visible setup latency of roughly one to two
  seconds before small LAN requests or microphone streaming began.

Candidate B: maintain persistent connections while the app is active.
- removes repeated handshake latency and better matches interactive agent use;
- requires framing, idle-connection detection, reconnection, and separate media
  and control flow handling.

Version `0.0.8-stage0` tested Candidate B using two warm HTTP/1.1 connections and
`Content-Length` response framing. The physical retest passed and the user
reported that text and audio startup felt responsive. Stage 1 therefore keeps
the two warm local connections.

Version `0.1.1-stage1` removes runtime socket waits from the interactive frame
loop. A bounded state machine advances control and audio work once per frame;
user commands can cancel an in-flight background event read. This is a required
property of any later transport, but it still does not accept the production
remote transport or the final number of connections.

Version `0.1.2-stage1.5` warms only the independent HTTP audio connection and
opens the pushed control link asynchronously. The legacy HTTP command socket is
retained as fixture/fallback code but is no longer pre-opened by the app.

### D-P10 — Stage 1 event delivery

**Status:** Proposed / local vertical-slice experiment

The historical Stage 1 client used persistent HTTP/1.1 for bounded commands and
cursor-based polling for newline-delimited JSON event batches. Per-session
sequence numbers allow a client to request events after its last applied cursor
and replay a bounded history after a short disconnect.

Version `0.1.1-stage1` makes those checks asynchronous and adaptive: about 100 ms
while working, 250 ms while waiting for approval, one second while idle, and an
exponential one-to-ten-second failure retry. This reduces idle log and network
volume without sacrificing the fake agent's current response cadence.

This choice reuses the low-latency socket behavior proven on physical 3DS
hardware and preserves cursor replay while the fake agent runs. Polling remains
a temporary local mechanism: its bounded batch/2 KiB response ceiling cannot be
assumed to sustain real adapter event rates. Pushed events are the next local
transport experiment.

Version `0.1.2-stage1.5` implements that experiment using a separate
development-only raw TCP control port with bounded newline-delimited JSON. The
3DS sends text captures and controls on the same persistent link that receives
acknowledgements, agent events, errors, and heartbeat replies. It resumes from
the last applied event cursor, retries one unacknowledged command with the same
ID, and reconnects with capped jittered backoff. The proven HTTP audio upload
remains separate.

This local framing is reversible and does not decide D-P11. Host tests pass;
physical reconnect, sleep, Wi-Fi, latency, and no-freeze evidence is pending.

### D-P11 — Secure bidirectional remote transport

**Status:** Proposed / resequenced behind R-010

WSS is the leading remote control candidate because relay hosting, proxy
traversal, and standard web infrastructure are real product requirements. A
small raw TLS protocol is the fallback, especially for a self-hosted topology.

Do not accept either framing until R-010 measures the shared TLS cost on Old 3DS:

- maintained library/toolchain path;
- handshake time, working memory, and reconnect/resumption behavior;
- non-blocking hostname resolution;
- certificate validation when the handheld RTC is incorrect;
- viable endpoint identity and credential rotation.

The WebSocket framing layer is small and reversible compared with TLS. The
current devkitPro package situation must be reconfirmed during R-010 rather than
assuming packaged WebSocket support. Separately decide whether control and
audio share one secure connection or use independent connections after measuring
backpressure, failure isolation, and the cost of a second TLS handshake.
