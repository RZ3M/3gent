# 3gent — Roadmap

This roadmap is ordered by **risk reduction**, not excitement.

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

**Status:** In progress; runtime pump host build passed, hardware check pending.

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

Exit criterion: pushed local events recover from a forced disconnect without UI
freezes, and R-010 has enough hardware evidence to accept or reject a secure
remote transport direction.

---

## Stage 2 — Codex adapter

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

Codex-specific wire objects must stop at the adapter boundary.

---

## Stage 3 — Voice-first local loop

Deliver:
- reliable push-to-talk through the bridge;
- transcription;
- transcript/error UX;
- latency measurements;
- retry;
- configurable transcription provider/local backend.

Complete the useful local voice→agent loop before adding remote transport risk.

---

## Stage 4 — QR pairing

Deliver:
- bridge-generated QR;
- camera scan;
- short-lived pairing bootstrap;
- device credential issuance;
- revocation;
- manual code fallback.

Security review required.

---

## Stage 5 — Remote relay

Deliver:
- authenticated bridge↔relay;
- authenticated 3DS↔relay;
- reconnect;
- rate limits;
- bounded media;
- minimal storage;
- remote session loop.

Do not ship a public relay until threat model and abuse controls are written.

---

## Stage 6 — Camera capture

Deliver:
- photo;
- preview;
- upload;
- attach to prompt.

---

## Stage 7 — Stylus capture

Deliver:
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
