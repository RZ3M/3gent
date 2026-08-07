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

**Status:** CORE LAN ROUND TRIP HARDWARE PASS / LATENCY RETEST TODO

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

**Status:** TODO

Test:
- Old 3DS;
- New 3DS if available;
- different screen brightness;
- QR sizes;
- camera distances;
- decoder candidates.

Do not block Stage 0 networking on QR work.

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

**Status:** TODO / high priority before remote beta

Test:
- available devkitPro libraries;
- supported modern cipher/protocol behavior;
- memory/CPU cost;
- certificate validation;
- connection reuse;
- timeouts.

Do not invent custom cryptography.

---

## R-011 — Codex rich integration

**Current finding:**
Codex provides a local `app-server` intended for rich clients. It supports structured thread/turn/item lifecycle, streaming events, approvals, and diff updates.

**Recommended first adapter path:**
- spawn `codex app-server --listen stdio://`;
- initialize;
- translate app-server events into 3gent events;
- keep Codex auth entirely on desktop.

**Research needed:**
- schema generation for installed Codex version;
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
