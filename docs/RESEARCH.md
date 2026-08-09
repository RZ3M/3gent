# 3gent — Research / Feasibility Tracker

**Rule:** Record measured results, not guesses.

## R-001 — Basic homebrew build

**Question:** Can we build and launch a minimal 3DSX with the current devkitPro toolchain?

**Status:** HOST BUILD PASSED / HARDWARE CORE PASS / MODEL RECORD TODO

**Success:**
- reproducible build command;
- app launches on target hardware;
- top/bottom screens render;
- button input works.

**Record:**
- host OS;
- devkitPro packages/tool versions;
- hardware model;
- build output;
- problems/workarounds.

**2026-08-07 initial environment record:**

- Host: macOS 26.5.2, Darwin 25.5.0, arm64.
- Tool inspection: `DEVKITPRO`, `DEVKITARM`, `dkp-pacman`,
  `arm-none-eabi-gcc`, and `3dslink` are not available on the host.
- Procedure: implemented the app from the current official devkitPro application
  template and attempted the documented `make` command.
- Result: the build stops immediately and explicitly because `DEVKITARM` is not
  set. No compiler result or hardware behavior is being claimed.
- Follow-up: install the official devkitPro macOS pacman package and `3ds-dev`
  group, build, and complete the checklist in `client-3ds/README.md`.

**2026-08-07 host build record:**

- Installed packages: devkitARM r68-1, libctru 2.7.0-1, 3dstools 1.3.1-3,
  and 3dslink 0.6.3-1.
- Procedure: `make clean`, then
  `make SERVER_HOST=192.0.2.1 SERVER_PORT=8080` in `client-3ds/`.
- Result: successful compilation and link with no warnings; generated
  `3gent.3dsx`, `3gent.smdh`, `3gent.elf`, and `build/3gent.map`.
- Size: ELF sections total 202,568 bytes; the generated 3DSX is approximately
  198 KiB.
- Independent check: the public GitHub workflow also built the 3DS client and
  passed all development-server tests using devkitPro's official current
  devkitARM container.
- Conclusion: reproducible host compilation is proven. Launch, rendering, input,
  and clean exit still require the physical-hardware checklist.

**2026-08-07 first hardware result:**

- The 3DSX launched through the Homebrew Launcher.
- Both screens rendered the expected Stage 0 interface and version label.
- The `A` input path opened the keyboard, proving that the application loop and
  button input were active.
- Clean `START` exit was not reported and remains to be checked.
- Hardware model still needs to be recorded.

---

## R-002 — Native software keyboard

**Question:** Can 3gent open the built-in software keyboard and retrieve useful UTF text?

**Status:** HARDWARE TEXT PASS / CANCEL AND NON-ASCII TODO

**Primary reference:** libctru software keyboard API and official example.

**Success:**
- keyboard opens;
- user enters text;
- result is displayed by 3gent;
- cancel behavior works.

**2026-08-07 buffered implementation record (superseded before hardware test):**

- The client uses `swkbdInit`, explicit Cancel/Send buttons,
  `swkbdInputText`, and `swkbdGetResult`, following the current official libctru
  software-keyboard example.
- Input is bounded to a 256-byte application buffer.
- Result: source implementation is ready, but keyboard launch, UTF-8 rendering,
  and cancellation have not been tested on a physical 3DS.

**2026-08-07 first hardware result:**

- The native keyboard opened and returned `hi`.
- The entered text rendered correctly on the top screen after the keyboard
  closed.
- Cancel behavior and non-ASCII UTF-8 input remain to be tested.

---

## R-003 — Local networking

**Question:** What is the simplest reliable way to send a small request from 3DS homebrew to a LAN server?

**Status:** CORE LAN AND LOW-LATENCY HARDWARE PASS / FAILURE PATHS TODO

Test:
- DNS if needed;
- raw socket or HTTP;
- connection timeout;
- reconnect;
- payload sizes;
- response rendering.

**First test:** plain local HTTP echo endpoint.

Do not use this experiment as the production remote security design.

**2026-08-07 implementation record:**

- Implemented a Python standard-library development server with `GET /health`
  and bounded `POST /echo` endpoints.
- Five host tests pass: health, UTF-8 echo, incremental stream fixture,
  unknown-path rejection, and rejection of request bodies larger than 4 KiB.
- Implemented a bounded raw-socket HTTP client using libctru's socket service.
  Connection, send, and receive waits are capped at five seconds with
  non-blocking sockets and `select`.
- Client prompt, request, raw HTTP response, and displayed response buffers are
  fixed-size. Network resources have explicit shutdown paths.
- Result: the host endpoint is proven; 3DS socket behavior, LAN reachability,
  timeout behavior, and on-screen rendering remain unverified until the client
  builds and runs on hardware.

**2026-08-07 first hardware result:**

- The 3DS at `10.0.0.144` reached the Mac at `10.0.0.196:8080`; the Python
  server accepted a TCP connection from source port 55059.
- Before sending HTTP, the client displayed `connect failed (-26: )`, closed the
  socket, and caused the server to report a connection reset.
- Inspection showed that libctru 2.7.0's `getsockopt(SO_ERROR)` returned the SOC
  service's raw `-26` (`EINPROGRESS`) value after `select` marked the socket
  writable and the Mac had accepted it. The client incorrectly treated that raw
  value as a final connection failure.
- Version `0.0.2-stage0` removes that unreliable post-connect `SO_ERROR` check.
  It proceeds after writability and relies on the bounded send/receive operations
  to surface an actual failure. Hardware retest is required before HTTP round
  trip success is claimed.

**2026-08-07 successful hardware retest:**

- Version `0.0.2-stage0` completed three separate HTTP round trips from the 3DS
  at `10.0.0.144` to the Mac at `10.0.0.196:8080`.
- The server received and returned `hi` (2 bytes),
  `whats upppppp im msging from my 3ds` (35 bytes), and `hey` (3 bytes).
- The user reported that the app displayed the responses correctly and was
  working well.
- Conclusion: repeated small text request/response over local Wi-Fi and response
  rendering are proven on hardware. Explicit server-offline timeout and retry
  behavior remain to be checked in Stage 0D.

**2026-08-07 latency feedback and warm-connection revision:**

- Hardware feedback measured about one second between submitting an `A` message
  and seeing it arrive at the laptop, and more than two seconds before the app
  showed microphone streaming after holding `R`.
- Inspection found that every text action and microphone session created a new
  non-blocking TCP socket and explicitly requested `Connection: close`. The
  microphone did not start until that setup returned, directly exposing the
  connection delay as input latency.
- Version `0.0.8-stage0` warms two verified HTTP/1.1 connections during app
  startup, dedicates one to control requests and one to audio, and reuses each
  after a `Content-Length`-framed response. Both ends enable `TCP_NODELAY` for
  the small Stage 0 writes.
- The audio send threshold drops from 4 KiB (roughly 125 ms of PCM) to 1 KiB,
  allowing each observed roughly 40 ms MIC service update to be transmitted
  immediately instead of waiting for three or four updates to accumulate.
- Microphone sampling now starts and becomes visible before the network stream
  setup fallback. The MIC ring can therefore absorb samples during a reconnect
  instead of shifting the beginning of the user's recording.
- The app displays startup warmup, request round-trip, total audio-start, and
  audio-link setup times. The focused trusted-LAN target is under 250 ms for a
  warm echo and total audio start; actual hardware values and idle/reconnect
  behavior remain to be tested.
- Host result: the development server reuses a single connection for repeated
  echo and audio requests in automated tests. The 3DS client builds with the
  configured toolchain; hardware confirmation remains required.

**2026-08-07 low-latency hardware retest:**

- The user tested the warm-connection revision on the physical 3DS and reported
  that both text messaging and microphone startup now respond quickly and work
  well.
- No exact on-screen millisecond values were recorded, so the qualitative pass
  does not establish a numeric latency budget.
- Conclusion: reuse of separate warm command and audio connections is proven
  useful on the tested LAN. Eleven-minute idle recovery, bridge restart, network
  loss, and the exact hardware model remain open measurements.

---

## R-004 — Incremental text display

**Question:** Can responses be appended and scrolled smoothly enough for agent output?

**Status:** CORE HARDWARE PASS

Measure:
- text wrapping;
- scrollback memory;
- update frequency;
- rendering cost;
- Old vs New 3DS if possible.

**2026-08-07 implementation record:**

- Version `0.0.3-stage0` adds `POST /stream`, which sends a bounded fixture in 28
  flushed pieces over roughly two seconds using ordinary HTTP with a known
  `Content-Length`.
- The 3DS client appends body bytes as they arrive and redraws after each receive,
  while retaining a fixed 2 KiB response buffer.
- Response display wraps at 48 bytes per line into at most 64 fixed lines. The
  top screen shows 22 lines at once; D-pad or Circle Pad Up/Down navigates older
  and newer lines.
- The client validates the final `Content-Length`, reports an early disconnect,
  retains any partial text, and exposes retry through `A` or `X`.
- Host result: the 3DSX builds cleanly and the five development-server tests pass.
- Host stream measurement: 1,424 response bytes delivered in 2.28 seconds across
  28 flushed application chunks.

**2026-08-07 hardware result:**

- The user confirmed that the deliberately streamed response rendered
  incrementally and completed successfully on the physical 3DS.
- Wrapped response navigation worked in both directions using individual input
  presses.
- Hardware feedback identified that holding a direction did not repeat, making
  longer navigation unnecessarily slow.
- Version `0.0.4-stage0` adds bounded held-direction repeat: one immediate step,
  an 18-frame initial delay, and one step every four frames while held. Opposing
  directions cancel repeat until only one direction remains active.
- The user completed the focused `0.0.4-stage0` retest and reported that held
  scrolling works correctly and the app is working well.
- Conclusion: incremental rendering, wrapping, and held scroll navigation are
  proven on hardware. Hardware model and subjective update smoothness remain to
  be recorded.

---

## R-005 — Microphone capture

**Question:** Can we reliably record push-to-talk audio into bounded memory/storage?

**Status:** SUSTAINED CAPTURE HARDWARE PASS / QUALITY AND FAILURE PATHS TODO

Determine:
- actual libctru microphone API to use;
- sample formats/rates supported;
- safe buffer strategy;
- maximum practical recording duration;
- whether to stream while recording or upload after release;
- quality on hardware;
- sleep/interruption behavior.

Start with raw/simple capture. Compression is a later optimization.

**2026-08-07 implementation record:**

- Primary reference: the locally installed official devkitPro microphone example
  from `3ds-examples` 20240917-1, checked against the MIC header in libctru
  2.7.0-1.
- API: `micInit`, `MICU_StartSampling`, `micGetLastSampleOffset`,
  `MICU_StopSampling`, and `micExit`. No microphone gain override is applied.
- Format: `MICU_ENCODING_PCM16_SIGNED`, mono, using
  `MICU_SAMPLE_RATE_16360`. Current libctru documents its actual rate as
  approximately 16,364.479 Hz; the standard WAV header uses the rounded integer
  value 16,364 Hz.
- Interaction: hold `R` to record and release to stop and upload. The bottom
  screen shows a duration and recent peak-level indicator. `Y` retries upload of
  the most recent completed capture.
- Bound: ten seconds, comprising at most 327,280 PCM bytes and a 44-byte WAV
  header. Capture stops automatically when full and does not upload until `R` is
  released.
- Microphone memory: a 196,608-byte, 0x1000-aligned looping service buffer plus
  a fixed 327,324-byte WAV buffer, for 523,932 bytes dedicated to this spike.
- Validation: `POST /audio` accepts at most 384 KiB, verifies non-empty mono
  PCM16 WAV data at 16,364 Hz using Python's standard library, and atomically
  saves `tools/dev-server/captures/latest.wav` for audible inspection.
- Failure behavior: MIC service/start/stop errors are visible on screen. A
  network upload failure retains the completed bounded WAV and exposes retry
  through `Y`.
- Host result: the 3DS client builds cleanly with devkitARM r68-1 and libctru
  2.7.0-1. Nine development-server tests pass, including valid audio saving,
  invalid/truncated WAV rejection, and oversized audio rejection.
- Hardware status: microphone initialization, level behavior, captured quality,
  exact duration/byte count, maximum-duration stop, upload latency, shell-close
  behavior, and Old/New 3DS model remain unmeasured. No hardware pass is claimed.

**2026-08-07 live-streaming revision:**

- Hardware feedback identified the fixed ten-second duration as unsuitable. The
  user requested direct microphone streaming rather than a larger whole-capture
  buffer.
- Version `0.0.6-stage0` opens one HTTP/1.1 chunked request before MIC sampling,
  drains the service ring every frame, and sends signed little-endian PCM in
  approximately 4 KiB chunks while `R` remains held.
- The client uses the 196,608-byte aligned MIC service ring plus one fixed 8 KiB
  outgoing buffer: 204,800 bytes dedicated to the audio path, independent of
  recording duration. The superseded whole-WAV buffer was removed.
- Releasing `R` stops MIC sampling, drains the final samples, sends the HTTP
  end-of-stream marker, and waits for the development server to finalize
  `latest.wav`.
- The default per-capture ceiling is five minutes (9,818,400 PCM bytes and a
  9,818,444-byte WAV). It bounds development disk/network use rather than client
  memory and is configurable on the server.
- The server streams PCM directly into a temporary WAV and atomically replaces
  `latest.wav` only after a valid, non-empty, complete PCM16 stream. Empty, odd,
  malformed, wrong-format, concurrent, and over-limit streams are rejected or
  discarded without replacing the last good capture.
- Failure behavior changed intentionally: because the 3DS no longer retains the
  whole capture, a failed live stream cannot be retried from handheld memory.
  The app stops with a visible error and the user records again.
- Host result: the client builds cleanly and twelve development-server tests pass,
  including multi-chunk WAV assembly and stream limit/format validation.
- Hardware status remains TODO: sustained streaming, visual level behavior,
  audio quality, Wi-Fi stalls, mid-stream server loss, shell close/resume, and
  actual Old/New 3DS model behavior are not yet measured.

**2026-08-07 first hardware result and diagnostic revision:**

- Five completed hardware streams produced 10,846, 1,278, 1,310, 1,310, and
  1,310 PCM bytes: approximately 331, 39, 40, 40, and 40 ms. The repeated 40 ms
  results occurred despite holding `R` for varying, substantially longer times.
- The latest 1,354-byte file was a structurally valid PCM16 mono WAV containing
  1,310 PCM bytes (655 frames at 16,364 Hz), so WAV finalization and clean HTTP
  stream completion worked. Its initial fragment was nearly silent (peak sample
  magnitude 66 of 32,768), which is consistent with only the first MIC update
  being captured and does not explain the duration failure.
- This rules out the button-release path and server duration calculation as the
  primary cause. The observed client write offset appeared to advance once and
  then remain unchanged while the request stayed open.
- Version `0.0.7-stage0` explicitly invalidates the ARM11 data-cache lines for
  the MIC service's shared write-position marker and each unread PCM range before
  consuming them. This is a hardware hypothesis, not yet a confirmed fix.
- The same revision checks `MICU_IsSampling` during capture and separately shows
  wall-clock hold duration, PCM duration, raw ring position, position-change
  count, service sampling state, and milliseconds since new data. A premature
  service stop now fails visibly instead of silently producing a short file.
- Host result: the diagnostic client builds cleanly with the configured
  devkitPro toolchain; all twelve development-server tests still pass. A focused
  physical retest is required before R-005 can be marked proven.

**2026-08-07 successful capture retest:**

- The user confirmed that version `0.0.7-stage0` continued capturing beyond the
  previous single 40 ms update and that streaming worked.
- The same test exposed a separate startup-latency issue: `R` had to remain held
  for more than two seconds before the UI entered its streaming state. This was
  caused by fresh TCP setup preceding MIC startup, not by the repaired sample
  capture loop, and is addressed separately by the `0.0.8-stage0` experiment.
- Sustained MIC sample capture is now a hardware pass. Intelligibility/gain,
  five-minute behavior, interruption, shell close/resume, network loss, and
  exact hardware model still require explicit results.

---

## R-006 — QR scanning

**Question:** Can the 3DS camera reliably scan a QR code displayed on common monitors?

**Status:** DECODER CHOSEN AND CROSS-VALIDATED ON THE HOST / HARDWARE PENDING

**2026-08-09 implementation record:**

- Decoder: quirc 1.2 (ISC), vendored unmodified into
  `client-3ds/third_party/quirc/` (D-021). devkitPro's `portlibs` ships no QR
  decoder, and this is the decoder other 3DS homebrew already relies on.
- Encoder: a byte-mode level-M encoder in the bridge, versions 1–10. The pairing
  payload lands at version 5–7 (37×37 to 45×45 modules).

Three things were settled off device rather than guessed:

1. **Encoder/decoder compatibility.** `tools/qr-check/` compiles the *vendored*
   quirc on the host and decodes the bridge's generated matrix. This caught a
   real defect immediately: the format-information reservation claimed nine
   modules per copy instead of eight, which silently shifted the whole codeword
   stream. Every symbol failed data ECC while still reporting a valid format —
   exactly the failure mode a unit test of the encoder alone would have missed.
   A second, independent matrix reader in `bridge/test/pairing.test.ts` guards
   the same property in `npm test`.
2. **Decode cost versus the frame loop.** A 400×240 decode is far more than one
   frame's budget on Old 3DS, so it runs on a worker thread one priority step
   below the interactive loop, with a single frame in flight. The viewfinder and
   the network pump keep running while a frame is analysed.
3. **Frame ownership.** The camera writes to the capture buffer only while a
   transfer is armed. Delivery and re-arming are therefore separate calls, which
   makes single-buffered capture race-free instead of merely usually-fine.

Memory: roughly 120 KB of decoder state plus a 192 KB RGB565 frame, allocated
only while the viewfinder is open and freed when it closes.

**2026-08-09 hardware finding — viewfinder froze at random.** The first physical
run paired successfully and exercised the whole flow, but the viewfinder
intermittently stopped updating, at no consistent frame count.

Cause: the streaming path armed transfers with `CAMU_SetReceiving` and waited
only on the receive event. The 3DS camera also raises a **buffer error
interrupt**, and when it does it stops feeding the armed transfer. Nothing
recovers on its own — the receive event simply never signals again, so the
stream froze wherever it happened to be. The one-shot photo path never hit this
because it lives for a single frame. devkitPro's own camera example watches this
interrupt and restarts capture; the streaming path did not.

Fix: acquire `CAMU_GetBufferErrorInterruptEvent`, check it before the receive
event, and on either that or 1.5 s of silence stop, clear, restart and re-arm.
A restart counter is surfaced on the pairing screen, so a hardware tester sees
"camera restarted N times" instead of guessing whether it froze. The stall
watchdog is deliberately independent of the documented error, because a frozen
viewfinder with no way out except cancelling is the worst outcome regardless of
which cause produced it.

**Still needs hardware:** confirmation that the recovery holds over a long
session and how often it fires; scan reliability against a real laptop panel at
varying brightness, distance and angle; Old versus New 3DS decode latency;
whether the terminal QR is usable at all or whether the SVG is required in
practice; and camera focus behaviour at the ~20 cm the interface suggests.

---

## R-007 — Camera photo prompt

**Question:** What photo resolution/encoding gives useful agent input without painful upload latency?

**Status:** TODO / post-MVP spike

---

## R-008 — Stylus drawing

**Question:** Should a sketch be sent as a bitmap or vector strokes?

**Status:** TODO / post-MVP spike

Start with whichever is simplest and most legible.

---

## R-009 — 3DS Wi-Fi compatibility

**Known from Nintendo support:**
- Nintendo 3DS-family networking is 2.4 GHz;
- 802.11b/g;
- supports multiple WEP/WPA/WPA2 personal modes depending on model;
- guest/captive networks can require browser sign-in and are not universally reliable.

**Implication:**
- hotspot workflow is first-class;
- do not assume a random modern 5 GHz-only guest network works;
- include network diagnostics.

**Still test:**
- iOS hotspot compatibility;
- Android hotspot compatibility;
- Windows/macOS/Linux hotspot behavior;
- WPA2/AES configurations;
- captive portal reality in 2026;
- DHCP/reconnect behavior.

---

## R-010 — TLS / secure remote transport

**Question:** Which maintained TLS/crypto path is practical on current 3DS homebrew?

**Status:** PRELIMINARY HOST RESEARCH / HARDWARE SPIKE TODO

Test:
- available devkitPro libraries;
- supported modern cipher/protocol behavior;
- memory/CPU cost;
- certificate validation;
- connection reuse;
- timeouts.

Do not invent custom cryptography.

**2026-08-08 package and architecture inspection:**

- The current devkitPro `3ds-curl` package definition is still 8.4.0-1. Its
  build disables the threaded resolver and pthreads and does not enable
  WebSocket support. curl made WebSocket support non-experimental in 8.11.0, so
  this package is not a production-ready shortcut to WSS.
- The current devkitPro `3ds-mbedtls` package definition is 2.28.8-1. Upstream
  now lists 3.6 and 4.1 as its maintained LTS branches; 2.28 is historical. A
  maintained custom 3DS build is therefore a likely spike candidate, not a
  decision already made.
- The curl package points at an SD-card CA bundle and disables its threaded
  resolver. The current 3gent socket client accepts only numeric IPv4. A remote
  relay needs a hostname, so non-blocking DNS is a shared prerequisite for WSS
  and raw TLS rather than a framing-specific detail.
- Ordinary certificate validity checks depend on the user-set 3DS RTC. The
  spike must test a wrong clock and evaluate a reviewed endpoint-identity plan;
  it must not simply disable validation. Pinning requires a rotation and
  recovery design rather than a permanent leaf-certificate assumption.
- WSS now leads at the product level because hosted relay and proxy traversal
  matter; raw TLS remains the fallback. This is still Proposed. The expensive
  question is whether a maintained TLS stack can meet Old 3DS handshake,
  memory, and reconnect targets.

**Required hardware measurements:**

- clean and resumed handshake duration on the tested 3DS model;
- peak/steady memory and resulting client binary size;
- TLS version and selected cipher suite;
- correct-certificate, wrong-host, expired-certificate, wrong-clock, and pin or
  trust-anchor rotation behavior;
- DNS lookup without a visible frame-loop stall;
- session resumption after Wi-Fi loss, lid close, and hotspot changes;
- one secure control connection versus independent control/media connections.

---

## R-011 — Codex rich integration

**Status:** INSTALLED INTERFACE INSPECTED / ADAPTER NOT YET IMPLEMENTED

**Current finding:**
Codex provides a local `app-server` intended for rich clients. It supports structured thread/turn/item lifecycle, streaming events, approvals, and diff updates.

**2026-08-08 installed-version inspection:**

- Installed CLI: `codex-cli 0.144.5` at `/opt/homebrew/bin/codex`.
- `codex app-server --listen stdio://` accepts newline-delimited JSON-RPC and
  successfully completes `initialize`.
- `codex app-server generate-json-schema` and `generate-ts` produce the
  authoritative bindings for this installed version.
- Required v2 methods exist: `thread/list`, `thread/resume`, `thread/start`,
  `turn/start`, `turn/interrupt`, and approval responses.
- Required notifications/requests exist: thread/turn state, agent-message
  deltas, item lifecycle, turn completion, diff updates, errors, command/file
  approvals, and permission approvals.
- Initial policy mapping: expose only single-use command/file acceptance plus
  decline/cancel. Do not expose session-wide acceptance, executable/network
  policy amendments, or permission-profile escalation on the handheld.
- Codex-native IDs and objects must stop at the adapter. The bridge needs stable
  3gent session IDs, bounded/coalesced text deltas, and compact diff summaries.

**Recommended first adapter path:**
- spawn `codex app-server --listen stdio://`;
- initialize;
- translate app-server events into 3gent events;
- keep Codex auth entirely on desktop.

**Implementation/research still needed:**
- session resume behavior;
- cancellation;
- approval mapping;
- image input;
- failure/restart behavior.

---

## R-012 — Herdr integration

**Current finding:**
Herdr exposes agent-aware control primitives over persistent terminal panes and can prompt/read/wait on supported agents.

**Research needed:**
- best stable API/CLI boundary;
- mapping Herdr workspace/tab/pane/agent to 3gent sessions;
- whether Herdr should expose sessions alongside direct adapters or act as one adapter.

---

## R-013 — Stage 1 local bridge and protocol

**Question:** Can the proven handheld paths support a production-shaped,
agent-agnostic local session loop before a real agent is introduced?

**Status:** HOST PASS / CORE HARDWARE PASS REPORTED / DETAILED CHECKLIST TODO

**2026-08-07 implementation record:**

- Implemented the desktop bridge in strict TypeScript on Node.js 22+ with no
  runtime dependencies.
- Protocol v1 uses explicit version and command-ID headers, bounded command
  bodies, 202 acknowledgements, NDJSON event envelopes, monotonically
  increasing per-session sequences, a 256-event replay window, and a 256-command
  deduplication window.
- The deterministic fake adapter exposes one session and exercises text deltas,
  working/idle/waiting states, interruption, approve/decline behavior, and the
  existing streamed microphone/WAV path.
- Eleven automated tests pass for version enforcement, session discovery,
  ordered event streaming/replay, text and audio command deduplication,
  approvals, interruption, WAV output, invalid cursors, oversized events, and
  safe-default versus verbose diagnostic logging.
- The `0.1.0-stage1` 3DS client sends protocol-v1 commands, polls up to three
  events roughly every 100 ms over its warm control connection, ignores unknown
  event types after advancing its cursor, renders text deltas, and exposes the
  Stage 1 controls. Event and display buffers remain fixed-size.
- A size audit found that one eight-event approval batch could reach 2,024 bytes,
  leaving too little margin in the 2 KiB client buffer. Protocol v1 now rejects
  event lines over 640 UTF-8 bytes, and the handheld requests at most three per
  poll, bounding a valid batch to 1,923 bytes including newline delimiters. The
  largest current fake event measured 598 bytes with a maximum-length prompt.
- Host result: the TypeScript build/tests pass and the client compiles with
  devkitARM r68-1 and libctru 2.7.0-1 for bridge address `10.0.0.196:8080`.
- A cursor older than retained history or ahead of a restarted in-memory stream
  is rejected explicitly. The 3DS then fetches a bounded session snapshot and
  visibly resynchronizes at its latest sequence instead of silently stalling.
- Unproven: physical event parsing/rendering, approval ergonomics, interruption
  timing, Stage 1 audio behavior, cursor resync behavior, long-idle socket
  recovery, server restart, sleep/resume, and Old versus New 3DS performance.

**2026-08-07 first Stage 1 hardware report:**

- The user ran the Stage 1 client and bridge together and reported that it works
  great.
- This is a qualitative core pass for the vertical slice. Individual results for
  interruption, both approval choices, long-idle reconnect, bridge restart,
  audio quality, sleep/resume, exact latency, and hardware model were not
  separately reported, so those checklist items remain open.
- Follow-up: the bridge now has an opt-in `--verbose` mode for the next focused
  tests. It logs exact text/JSON and outbound event envelopes, while PCM remains
  summarized as chunk and total byte counts. Default logs continue to omit full
  prompt content.

**2026-08-08 non-blocking runtime implementation record:**

- Version `0.1.1-stage1` replaces runtime five-second socket waits with bounded
  control and audio state machines advanced once per frame. Connect, header/body
  send, response receive, PCM chunk send, and audio finalization can all remain
  in progress while the app continues scanning buttons and drawing.
- One bounded control operation is active at a time. A text capture, approval,
  or interrupt can cancel a background event read so a user action is not queued
  behind a stale poll. Unique command IDs and bridge deduplication still protect
  accepted mutations.
- Event checks retain the protocol-v1 sequence cursor and three-event/2 KiB
  bounds, but adapt from roughly 100 ms while working to 250 ms for approvals
  and one second while idle. Failure retries back off from one to ten seconds.
- The bridge-to-3DS direction remains functional: `assistant.text.delta`, state,
  approval, completion, and error events are applied and rendered separately
  from 3DS-to-bridge capture/control acknowledgements.
- Audio keeps the fixed 8 KiB client staging buffer and a single bounded network
  chunk queue. Releasing `R` stops sampling immediately, then drains and
  finalizes asynchronously instead of holding the frame loop for the server
  response.
- Host result: the client compiles cleanly with devkitARM r68-1 and libctru
  2.7.0-1 for `10.0.0.196:8080`. Hardware responsiveness, stream correctness,
  and reconnect behavior are not claimed until the updated checklist passes.
- Size result: the linked ELF reports 205,196 bytes of text, 7,808 bytes of
  data, and 58,728 bytes of BSS; the packaged 3DSX is approximately 223 KiB.

**Conclusion:** the host-side vertical slice is reproducible, bounded, and has
a qualitative core hardware pass. Complete the focused `0.1.1-stage1`
reliability checks, then prove local push and R-010 before beginning the Codex
adapter.

---

## R-014 — Local pushed bidirectional control

**Question:** Can the 3DS receive agent output immediately without periodic HTTP
event requests while retaining bounded memory, replay, command safety, and a
responsive frame loop?

**Status:** FAKE-BACKEND HARDWARE FLOW PASS / REAL TRANSCRIPTION CHECKLIST PENDING

**2026-08-08 implementation record:**

- Version `0.1.2-stage1.5` replaces the handheld event-poll loop with one
  long-lived development-only raw TCP control connection on port 8081. The HTTP
  service on port 8080 remains for audio and host fixtures.
- Control framing is newline-delimited UTF-8 JSON with a 4 KiB per-frame/input
  bound. Existing protocol-v1 event envelopes and command acknowledgements are
  wrapped rather than redefined.
- Text captures, interrupt commands, and approval responses travel 3DS→bridge;
  acknowledgements, errors, session state, approvals, and assistant deltas are
  pushed bridge→3DS on the same link.
- The 3DS retains at most one mutating command until acknowledgement. If the
  connection drops after send, the exact command frame and ID are retried;
  bridge deduplication prevents a previously accepted command from running
  twice.
- The client resumes from its last applied event sequence. Expired or ahead
  cursors receive a bounded `resync.required` snapshot and a visible output-gap
  marker before reconnect.
- Client liveness sends a ping after three seconds without inbound traffic and
  reconnects after eight seconds without any response. Retry starts around 250
  ms, doubles to a ten-second cap, and adds ±20 percent jitter. The bridge drops
  peers that send no traffic for twelve seconds.
- The bridge event history now has both a 256-event cap and a 128 KiB cap.
  Per-client delivery relies on socket backpressure and replay instead of an
  unbounded event queue.
- Ordinary verbose logs show meaningful push frames but suppress ping/pong.
  `--verbose-polls` remains the explicit noisy diagnostics mode and also shows
  push heartbeats.
- The native software keyboard is modal and stops the application frame loop.
  The client now closes the push link before opening it and resumes from the
  applied cursor afterward instead of predictably hitting heartbeat timeout
  during normal typing.
- Explicit bridge-only hardware diagnostics can blackhole outbound push frames,
  drop one post-execution acknowledgement, and slow fake deltas. They are off by
  default and print a warning when enabled.
- Host result: strict TypeScript build plus 23 automated tests pass, including
  immediate push, reconnect deduplication, invalid-cursor resync, oversized
  frame rejection, heartbeat cleanup, and byte-budget eviction.
- Handheld build result: devkitARM r68-1/libctru 2.7.0-1 compile succeeds. The
  ELF contains 208,516 bytes text, 7,808 bytes data, and 73,488 bytes BSS; the
  packaged 3DSX is 232,184 bytes.

**Not yet proven:** real 3DS connection establishment, event latency, heartbeat
behavior over 2.4 GHz Wi-Fi, bridge restart, command replay at the failure
boundary, lid close/resume, simultaneous audio/event activity, Old 3DS memory
and CPU behavior, and UI responsiveness during all failures.

**Conclusion:** the local host/client implementation is ready for a focused
physical test. This proves push mechanics only; it does not choose raw TCP for
the encrypted self-hosted relay. R-010 still gates that decision.

---

## R-015 — Installed Codex app-server adapter

**Question:** Can the desktop bridge control and observe real Codex tasks through
the supported local structured interface without leaking Codex wire objects to
the handheld?

**Status:** HOST PASS / PHYSICAL 3DS TEST PENDING

**2026-08-08 implementation record:**

- The installed `codex-cli 0.144.5` accepted
  `codex app-server --listen stdio://`. Its generated v2 TypeScript/JSON schema
  was inspected at implementation time instead of copying an old snapshot.
- The bridge initializes the stdio server, correlates bounded concurrent JSON-RPC
  requests, enforces line and pending-request limits, handles split/coalesced
  input, and fails pending work on timeouts or child exit.
- Supported translation covers `thread/list`, `thread/start`, `thread/resume`,
  `turn/start`, `turn/interrupt`, thread/turn state, streamed agent-message
  deltas, completion, diff updates, errors, and command/file approvals.
- Codex thread UUIDs are hashed into stable opaque `ses_codex_` identifiers.
  Text deltas are split on UTF-8 character boundaries to stay inside the 640-byte
  event limit; raw diffs become file/addition/deletion counts.
- Approval responses are limited to one-shot accept, decline and cancel. Extended
  permission-profile requests are rejected instead of granting broader policy.
- A live read-only smoke test launched the installed app-server and returned 24
  recent real Codex tasks through `/v1/sessions`; no task was modified.
- Host result: strict TypeScript build plus 37 automated tests pass. The 3DS
  task chooser build succeeds with devkitARM r68-1/libctru 2.7.0-1. The ELF has
  212,156 bytes text, 7,808 bytes data and 74,576 bytes BSS; the 3DSX is 235,876
  bytes.

**Not yet proven:** session chooser usability, real response cadence, approval
round trips, interruption, diff summaries and reconnect/resume on physical 3DS
hardware.

**Conclusion:** the real local Codex observation/control loop is ready for the
focused Stage 2 physical checklist. Voice remains intentionally unsupported on
this adapter until transcription and transcript review are implemented.

---

## R-016 — Voice transcription with handheld review

**Question:** Can streamed 3DS microphone audio become a reviewed text capture
without coupling transcription to an agent or automatically starting a turn?

**Status:** HOST PASS / PHYSICAL 3DS TEST PENDING

**2026-08-08 implementation record:**

- The bridge saves the proven bounded PCM stream as WAV, then invokes a selected
  transcription backend. A no-shell local command backend appends the WAV path
  as its final argument; an optional hosted backend uses the standard multipart
  `/v1/audio/transcriptions` API and keeps its key on the laptop.
- Transcript output is trimmed, non-empty, and limited to 1,600 UTF-8 bytes so
  the complete reviewed text and escaped push command remain bounded on the 3DS.
- The bridge emits `capture.transcript.delta` followed by `capture.transcribed`.
  It does not call an agent adapter. The handheld reconstructs and displays the
  text, then maps `A` to send, `Y` to native-keyboard edit, and `B` to cancel.
- Audio finalization remains non-blocking in the frame loop and gets a separate
  120-second transcription-response deadline. Push heartbeats and agent events
  continue on the independent control connection.
- Host result: strict TypeScript build plus 40 tests pass, including local
  command failure/output bounds and a local OpenAI-compatible multipart fixture.
- Handheld build result: devkitARM r68-1/libctru 2.7.0-1 succeeds. The ELF has
  213,692 bytes text, 7,808 bytes data and 80,472 bytes BSS; the 3DSX is 237,424
  bytes.

**Not yet proven:** real transcription latency/accuracy, 120-second finalization
behavior, transcript reconstruction, edit/cancel/send controls, and long or
non-ASCII transcripts on physical 3DS hardware.

**Conclusion:** Stage 3 is ready for physical testing. The same transcript event
contract works with local/self-hosted and hosted recognizers.

**2026-08-08 initial hardware update:** With the `0.6.1-hwtest` client and fake
adapter, microphone audio uploaded, produced the mock reviewed transcript, and
sent successfully. Real transcription, edit/cancel, long-input and failure-path
items remain pending.

---

## R-017 — Self-hosted reverse relay feasibility

**Question:** Can a self-hosted server carry the complete bidirectional and media
protocol when the laptop is behind NAT and opens only outbound connections?

**Status:** HOST PASS / REMOTE 3DS TEST PENDING / SECURITY INTENTIONALLY INCOMPLETE

**2026-08-08 implementation record:**

- The relay exposes separate public HTTP/media and pushed-control ports, plus
  separate token-authenticated bridge uplink ports. The laptop offers four
  HTTP/media tunnels and one pushed tunnel and replenishes them with 250 ms to
  ten-second reconnect backoff.
- Pairing is socket-for-socket and bounded to eight waiting clients/uplinks per
  channel with 15-second waiting and five-second activation timeouts. The relay
  never parses agent protocol content and stores no provider credential.
- Deterministic tests pass repeated HTTP sockets, bidirectional push bytes, the
  real bridge `/health` response and a real pushed `connection.hello` through
  the reverse tunnel.
- The 3DS uses the unchanged client build with the relay's numeric IPv4 and
  public ports, so text, events, voice and photos follow the same paths.
- Per explicit scope, the public 3DS side has no TLS or device auth. The relay
  requires `--unsafe-public` and is not suitable for sensitive or public use.

**Conclusion:** the self-hosted remote topology is host-proven and ready for a
disposable cross-network hardware test. It does not close R-010 or D-P11.

---

## R-018 — 3DS photo capture and Codex attachment

**Question:** Can the handheld capture, preview, upload and attach a bounded
photo without expanding the client into an image editor?

**Status:** INITIAL HARDWARE FLOW PASS / IMAGE QUALITY AND CODEX CHECKLIST PENDING

**2026-08-08 implementation record:**

- The client follows the installed official devkitPro/libctru image example for
  outer-camera activation, 400×240 RGB565 receive, shutter and cleanup. One
  192,000-byte buffer is allocated only while the capture/preview/upload flow is
  active.
- `L` opens the capture flow; the top screen previews the real buffer, `A`
  accepts and `B` cancels. Upload reuses the bounded 8 KiB media queue and pumps
  control heartbeats/events every frame.
- The bridge requires exactly 192,000 RGB565 bytes, writes a 66-byte bitfield BMP
  header plus pixels atomically, and retains one pending photo per session.
- The next successful prompt consumes the photo. Codex receives it as app-server
  `localImage`; the fake adapter exposes the same attachment behavior in tests.
- Host result: the full strict bridge suite passes 44 tests. Handheld clean build
  succeeds with devkitARM r68-1/libctru 2.7.0-1. The ELF has 219,332 bytes text,
  7,808 bytes data and 80,776 bytes BSS; the 3DSX is 243,212 bytes.

**Not yet proven:** camera service coexistence with MICU, capture orientation and
color masks on hardware, preview correctness, upload latency, BMP appearance,
and real Codex visual interpretation.

**Conclusion:** photo functionality is ready for the physical checklist; sketch
capture remains deliberately excluded.

**2026-08-08 initial hardware update:** The `0.6.1-hwtest` client captured and
sent a photo successfully through the fake bridge. The full orientation/color,
cancel, cross-session, retry and real Codex attachment checks remain pending.

---

## R-019 — Functional-core startup discovery on hardware

**Question:** Does a failed initial session lookup remain observable and
recoverable instead of looking like an application crash?

**Status:** CORRECT-ENDPOINT DISCOVERY PASS / OFFLINE RETRY RETEST PENDING

**2026-08-08 hardware report and diagnosis:**

- The first `0.6.0-hwtest` launch returned directly to Homebrew Launcher before
  showing the task picker. The verbose bridge received no request and Luma had
  no ARM11 dump, proving this was a controlled exit rather than an exception.
- The installed 3DSX was inspected over FTP and contained `192.0.2.1`, the
  intentionally unreachable documentation address used by the clean host build.
  The laptop's active LAN address was `10.0.0.196`.
- Session discovery timed out against the wrong address. `choose_session()` then
  returned false and main shut down normally, which was indistinguishable from
  a crash to the user.
- Version `0.6.1-hwtest` keeps the task-discovery error on screen and accepts
  `A` or `X` to retry; only `START` exits. The client is rebuilt with the active
  numeric laptop address for the focused retest.

**Conclusion:** compile-time endpoint mistakes must be visible and retryable.
The physical retest must confirm task discovery and pushed connection using the
correct embedded address.

---

## R-020 — Console framebuffer flicker

**Question:** Can the console UI remain stable when no visible state changed?

**Status:** ROOT CAUSE CONFIRMED / FIX BUILDS / PHYSICAL RETEST PENDING

**2026-08-08 hardware report and diagnosis:**

- Version `0.6.1-hwtest` reached the fake task picker and the underlying text,
  photo and voice flows worked, but both screens flashed continuously while the
  picker was idle.
- A hardware photo showed partially rendered alternating console frames. The
  picker cleared, redrew and swapped framebuffers on every loop iteration even
  when selection and status were unchanged. The main loop also swapped idle
  buffers without requiring a redraw.
- Version `0.6.2-hwtest` marks console frames dirty on redraw, swaps only dirty
  main frames, and leaves a stable front buffer while idle. Session loading and
  retry screens draw once; the picker redraws only when selection or status
  changes.

**Conclusion:** framebuffer swaps are presentation work, not a frame-loop
heartbeat. The physical retest must cover idle task picker, idle main view,
streamed response updates, held scrolling, microphone meters and photo preview.

---

## R-021 — GPU interface on the handheld

**Question:** Can the thin client render a designed GPU interface with citro2d
while keeping the existing non-blocking network, microphone and camera paths
intact on Old 3DS?

**Status:** HOST BUILD PASSED / LAYOUT REVIEWED OFF DEVICE / HARDWARE PENDING

**2026-08-08 implementation record:**

- Host: macOS 26.5.2, Darwin 25.6.0, arm64. Toolchain: devkitARM r68,
  libctru 2.7.0, citro3d and citro2d from the installed `3ds-dev` group.
- `client-3ds/source/ui.c` replaces all `PrintConsole` output. `main.c` keeps
  every protocol, audio, camera and reconnect behavior unchanged and now
  populates a `UiModel` once per frame.
- The build is clean under the project's existing `-Wall -Wextra -Werror`.

Three questions had to be answered rather than guessed:

1. **Text measurement.** citro2d can word-wrap at draw time, but scrollback needs
   line-level control. Advances are read from the shared system font through
   `C2D_FontGetCharWidthInfo`, cached for ASCII at start-up, and used for greedy
   wrapping. The wrapped line table is cached and recomputed only when the text
   itself changes. Callers pass reused fixed buffers, so pointer identity and
   length cannot prove the content is unchanged; the cache key includes an
   FNV-1a hash of the text. That costs a few kilobytes of hashing per frame and
   removes any obligation on `main.c` to invalidate anything.
2. **Camera preview.** `camera_capture_draw_preview` wrote RGB565 straight into
   the top framebuffer, which the GPU now owns. The frame is instead tiled into
   a 512×256 `GPU_RGB565` texture in software (8×8 tiles, Morton order) and
   drawn as a `C2D_Image`.
3. **Texture v-axis orientation.** Whether sampled `v` runs bottom-up over
   stored texture rows decides if the preview appears upside down, and it cannot
   be verified off device. Rather than hard-code a convention, `ui.c` asks
   `C2D_FontCalcGlyphPos` for glyph 0 at start-up. That glyph always occupies the
   top-left cell of the first system-font sheet, so its reported top texture
   coordinate identifies the convention on the installed libctru. Both branches
   are implemented.

**Off-device review:** `tools/ui-preview/` compiles the same `ui.c` against a
recording backend that re-emits every draw call as SVG. It found two real
defects before any hardware run: translucent fills in `ui_panel_outlined`
composited against the filled border panel instead of the page, and the
recording card's level trace overlapped the record indicator while its caption
overflowed the card. Both are fixed. Glyph advances are approximated on the
host, so text fit still needs a hardware pass.

**Conclusion:** citro2d is the right layer for this product. Pending hardware
evidence: sustained frame rate and battery behavior during idle, streaming and
recording; text legibility at the chosen scales on a physical panel; camera
preview orientation; and the modal software keyboard returning cleanly with the
GPU still owned by citro3d.

---

## R-022 — Task switching, touch, and context-aware controls

**Question:** Can the handheld carry more than one coding-agent task at a time —
showing which of them needs the user, and moving between them — using the
hardware the 3DS actually has, without a new bridge capability?

**Status:** HOST BUILD PASSED / LAYOUT REVIEWED OFF DEVICE / HARDWARE PENDING

**2026-08-09 implementation record:**

- Host: macOS 26.5.2, Darwin 25.6.0, arm64. Toolchain: devkitARM r68,
  libctru 2.7.0. Clean under the project's `-Wall -Wextra -Werror`.
- No bridge or protocol change was required. `/v1/sessions` already returns
  `state`, `pendingApprovalId` and `lastSequence` per session, which is exactly
  what a cross-task attention indicator needs.

Four questions had to be answered rather than guessed:

1. **Where touch geometry lives.** Putting hit rectangles in `main.c` would let
   them drift from what `ui.c` draws, silently, with no way to catch it off
   device. `ui_hit_test` therefore lives in `ui.c` and calls the same layout
   helpers the renderer calls (`ui_action_rect`, `ui_rail_first`,
   `ui_task_list_first`, `ui_hit_row`). It stays pure, so the preview harness
   still compiles the file unmodified.
2. **Touch position after release.** Firing on release is what makes a mis-aimed
   stylus recoverable, but `hidTouchRead` reports the origin once contact ends,
   and the origin is a live target — the first task tab. The last held point is
   retained and the release is judged against that. This was found by reasoning
   about the libctru contract, not on screen, and is the single most likely
   place for a hardware surprise.
3. **Whether cross-task state can be live without fighting the control slot.**
   Exactly one control request may be in flight. The background refresh starts
   only when the slot is idle, runs at most every five seconds, and every
   foreground request cancels it first. It is skipped entirely while recording.
4. **What a task switch costs.** Switching stops the pushed link, POSTs
   `/resume`, resets the cursor to zero and restarts the link, so the bridge
   replays that task's history. Replay exposed a pre-existing defect: the 4 KiB
   response buffer filled and then discarded everything that followed, meaning a
   switched-to task would show its oldest output and stop. `append_response` now
   drops whole lines from the front instead.

**Off-device review:** `tools/ui-preview/` covers twenty states, including four
that exist only because of this change (busy rail with the scroll cluster live,
windowed task list, approval under a stylus, and a touched menu row). It caught
four real defects: a four-across action label truncating to "Ne...", a status
band too short for three lines of text, "Start a new task" offered while the
bridge was unreachable, and the photo upload naming itself twice.

**Conclusion:** multi-task monitoring needs no protocol work; it needed the
bottom screen to stop being a decorative second display. Pending hardware
evidence: whether 70 px action buttons and 32 px task tabs are comfortable
stylus targets on a physical panel; whether the 450 ms approval arming delay
feels protective or obstructive; whether the five-second background refresh is
visible in battery or frame rate; and whether a task switch's stop/resume/replay
round trip is fast enough to feel like switching rather than reconnecting.

---

## Experiment template

When closing a research item, add:

```text
Date:
Hardware:
Host OS:
Tool versions:
Procedure:
Result:
Measurements:
Problems:
Conclusion:
Decision impacted:
Follow-up:
```
