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

**Status:** Host implementation complete; physical 3DS vertical-slice test pending.

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

Exit criterion: the physical checklist in `client-3ds/README.md` passes through
text, interruption, both approval choices, and sustained audio.

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
