# 3gent — Research / Feasibility Tracker

**Rule:** Record measured results, not guesses.

## R-001 — Basic homebrew build

**Question:** Can we build and launch a minimal 3DSX with the current devkitPro toolchain?

**Status:** HOST BUILD PASSED / HARDWARE TODO

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

---

## R-002 — Native software keyboard

**Question:** Can 3gent open the built-in software keyboard and retrieve useful UTF text?

**Status:** IMPLEMENTED / HARDWARE TODO

**Primary reference:** libctru software keyboard API and official example.

**Success:**
- keyboard opens;
- user enters text;
- result is displayed by 3gent;
- cancel behavior works.

**2026-08-07 implementation record:**

- The client uses `swkbdInit`, explicit Cancel/Send buttons,
  `swkbdInputText`, and `swkbdGetResult`, following the current official libctru
  software-keyboard example.
- Input is bounded to a 256-byte application buffer.
- Result: source implementation is ready, but keyboard launch, UTF-8 rendering,
  and cancellation have not been tested on a physical 3DS.

---

## R-003 — Local networking

**Question:** What is the simplest reliable way to send a small request from 3DS homebrew to a LAN server?

**Status:** SERVER VERIFIED / 3DS CLIENT HARDWARE TODO

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
- Four host tests pass: health, UTF-8 echo, unknown-path rejection, and rejection
  of request bodies larger than 4 KiB.
- Implemented a bounded raw-socket HTTP client using libctru's socket service.
  Connection, send, and receive waits are capped at five seconds with
  non-blocking sockets and `select`.
- Client prompt, request, raw HTTP response, and displayed response buffers are
  fixed-size. Network resources have explicit shutdown paths.
- Result: the host endpoint is proven; 3DS socket behavior, LAN reachability,
  timeout behavior, and on-screen rendering remain unverified until the client
  builds and runs on hardware.

---

## R-004 — Incremental text display

**Question:** Can responses be appended and scrolled smoothly enough for agent output?

**Status:** TODO

Measure:
- text wrapping;
- scrollback memory;
- update frequency;
- rendering cost;
- Old vs New 3DS if possible.

---

## R-005 — Microphone capture

**Question:** Can we reliably record push-to-talk audio into bounded memory/storage?

**Status:** TODO

Determine:
- actual libctru microphone API to use;
- sample formats/rates supported;
- safe buffer strategy;
- maximum practical recording duration;
- whether to stream while recording or upload after release;
- quality on hardware;
- sleep/interruption behavior.

Start with raw/simple capture. Compression is a later optimization.

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
