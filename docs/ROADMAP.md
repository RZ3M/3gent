# 3gent — Roadmap

This roadmap is ordered by **risk reduction**, not excitement.

The consolidated physical acceptance procedure for the current functional core
is `docs/HARDWARE_TEST_CHECKLIST.md`.

## Stage 0 — Prove the 3DS boundaries

**Status:** Core feasibility complete on hardware; failure-path hardening remains.

### 0A. Hello 3gent

Deliver:
- reproducible 3DS homebrew build;
- top/bottom-screen UI;
- input loop;
- visible version/build info.

### 0B. Native keyboard

Deliver:
- Type action;
- native software keyboard;
- returned text displayed locally.

### 0C. LAN text round trip

Deliver:
- tiny local dev server;
- 3DS sends typed text;
- server responds;
- 3DS shows response.

**Milestone:** type `hello` on 3DS → computer receives it → response appears on 3DS.

### 0D. Incremental output

Deliver:
- response chunks or simulated streaming;
- scrolling/wrapping;
- disconnect/retry indicator.

### 0E. Microphone spike

Deliver:
- push-to-talk;
- bounded recording;
- save/upload proof;
- basic recording UI.

### Exit criterion

We know whether the core client is technically viable on real hardware.

---

## Stage 1 — Local fake-agent vertical slice

**Status:** Host complete and core physical run reported working; detailed
hardware/reliability checklist remains.

Build a minimal local bridge abstraction.

Flow:

```text
3DS text/voice → bridge → fake agent → structured events → 3DS
```

Deliver:
- TypeScript/Node desktop bridge;
- versioned agent-agnostic protocol;
- session ID;
- working/idle status;
- text delta events;
- interrupt;
- fake approval request.

Do not integrate real coding agents until the 3gent protocol shape feels sane.

Implemented on the host:

- protocol-v1 headers, command IDs, acknowledgements, errors, and event envelope;
- bounded per-session replay and command deduplication;
- deterministic fake text, approval, interruption, and audio behavior;
- strict TypeScript build and automated bridge tests;
- Stage 1 3DS build with bounded event parsing and the full control mapping.
- non-blocking per-frame runtime network progress for commands, event checks,
  microphone chunks, and audio finalization;
- adaptive local event checks with user-command priority and bounded retry
  backoff.

Exit criterion: the physical checklist in `client-3ds/README.md` passes through
text, interruption, both approval choices, and sustained audio.

---

## Stage 1.5 — Push and secure-transport feasibility

**Status:** Local push implemented and host-tested; physical push/reconnect and
R-010 hardware checks pending.

This stage is intentionally before a real adapter because a blocking or
unsupportable connection layer would invalidate later work.

Deliver in this risk order:

1. Prove the `0.1.1-stage1` runtime pump stays responsive during half-open,
   bridge-offline, audio-finalization, and reconnect cases on physical hardware.
2. Replace local event polling with pushed events while retaining sequence
   cursor resume, bounded queues, heartbeat/liveness, reconnect with jitter, and
   command deduplication.
3. Run R-010 early: maintained TLS candidate, Old 3DS handshake time/memory,
   non-blocking DNS, clock/certificate behavior, session resumption, and hotspot
   behavior.
4. With those measurements, choose WSS or raw TLS framing and one versus two
   secure connections. WSS leads; raw TLS remains the fallback.

All test transports must remain bidirectional: captures and controls go to the
bridge; agent state, response deltas, approvals, and errors come back to the 3DS.

Host implementation in `0.1.2-stage1.5`:

- a dedicated development-only raw TCP control port (`8081` by default);
- bounded newline-delimited JSON in both directions;
- immediate pushed events with no 3DS event polling;
- last-applied cursor replay and visible resync on expired/ahead history;
- one durable command retried by the same ID after reconnect;
- three/eight-second client heartbeat and twelve-second bridge idle cleanup;
- 250 ms to ten-second jittered reconnect backoff;
- 256-event and 128 KiB event-history bounds with socket backpressure;
- existing independent HTTP microphone stream retained for isolation.

This local raw TCP experiment does not accept raw TCP as the remote framing.

Exit criterion: pushed local events recover from a forced disconnect without UI
freezes, and R-010 has enough hardware evidence to accept or reject a secure
remote transport direction.

---

## Stage 2 — Codex adapter

**Status:** Host implementation complete; physical 3DS checklist pending.

Use Codex app-server on the desktop side.

Deliver:
- start/resume a Codex thread as product policy allows;
- send prompt;
- render streamed agent text;
- map turn state;
- interrupt;
- surface approval request;
- respond to approval;
- show diff summary if practical.

This is an observation-and-control loop, not send-only integration: Codex turn
events and assistant output must remain visible on the 3DS throughout the turn.

Implemented in `0.2.0-stage2`: supported stdio app-server startup, installed
schema inspection, bounded JSON-RPC, recent task discovery, new/resumed tasks,
opaque session IDs, prompt/interrupt, streamed response and state translation,
compact diff counts, one-shot command/file approvals, unsupported permission
grant rejection, and a handheld task chooser.

Codex-specific wire objects must stop at the adapter boundary.

---

## Stage 3 — Voice-first local loop

**Status:** Host implementation complete; physical transcript-review checklist pending.

Deliver:
- reliable push-to-talk through the bridge;
- transcription;
- transcript/error UX;
- latency measurements;
- retry;
- configurable transcription provider/local backend.

Accepted behavior: show the transcript on the 3DS after recording and require a
separate explicit send action; allow cancel/edit where practical.

Complete the useful local voice→agent loop before adding remote transport risk.

Implemented in `0.3.0-stage3`: bounded WAV transcription through either a local
command backend or the OpenAI audio transcription API, transcript delta events,
a 1,600-byte review bound, explicit handheld send/edit/cancel controls, and a
120-second non-blocking finalization deadline. Audio never starts an agent turn
until the user sends the reviewed transcript.

---

## Stage 4 — QR pairing

**Status:** Host and client implementation complete; physical scan and
persistence checks pending. Enforcement deliberately deferred.

Delivered in `0.8.0-pairing`:
- bridge-generated QR, printed to the terminal and written as an SVG;
- a start screen that owns endpoint selection, replacing the compile-time host;
- camera scan with off-thread decoding (vendored quirc, D-021);
- short-lived single-use pairing bootstrap carrying no credential;
- revocable device credential issuance, stored as a hash on the bridge;
- `--list-devices` and `--revoke-device`;
- manual code fallback, including pasting the full pairing URL.

Not delivered, by decision rather than omission:
- **token enforcement by default.** `--require-pairing` exists and is off. A
  bearer token over a plaintext link is an identity, not an access control; it
  becomes mandatory with secure transport (D-022, D-P11).
- **endpoint identity binding.** The payload reserves `f` for it.
- **pairing through the relay.** The QR carries a direct host and port.

Exit criterion: a physical 3DS scans a bridge QR from a normal laptop screen,
the credential survives a relaunch, and R-006 has real scan-distance evidence.

Security review still required before any of this is called release-ready.

---

## Stage 5 — Remote relay

**Initial distribution:** self-hosted.

**Status:** Plaintext self-hosted hardware-test path implemented; production
authentication/TLS and physical remote-network tests pending.

Deliver:
- authenticated bridge↔relay;
- authenticated 3DS↔relay;
- reconnect;
- rate limits;
- bounded media;
- minimal storage;
- remote session loop.

Do not ship a public relay until threat model and abuse controls are written.

The current reversible test implementation uses four relay ports: public HTTP
media/session traffic, public pushed control, authenticated outbound bridge HTTP
uplinks, and an authenticated outbound bridge push uplink. A bounded pool lets
the laptop remain behind NAT. It refuses to start without `--unsafe-public` and
is not accepted as the production transport.

---

## Stage 6 — Camera capture

**Status:** Host/client implementation complete; physical 3DS camera test pending.

Deliver:
- photo;
- preview;
- upload;
- attach to prompt.

Implemented: outer camera RGB565 capture at 400×240, on-device preview,
cancel/accept, bounded chunked upload, bridge-side BMP construction, one pending
photo per session, consume-on-next-prompt semantics, and Codex `localImage`
translation.

---

## Stage 7 — Stylus capture

**Status:** Outside the current product goal.

If revisited later:
- sketch canvas;
- undo/clear;
- send;
- agent receives image or vector capture.

---

## Stage 8 — More adapters

Candidates:
- Herdr;
- Claude Code;
- others.

Adapter priority should be driven by stable integration surfaces and contributor demand.

---

## Stage 9 — Product polish

- session switcher;
- notifications where platform permits;
- compact diff view;
- network diagnostics;
- onboarding;
- self-host docs;
- contributor docs;
- release packaging.
