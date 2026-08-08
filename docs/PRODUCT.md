# 3gent — Product Specification

**Status:** Draft v0.2
**Audience:** contributors, maintainers, coding agents

## 1. Vision

3gent turns a modded Nintendo 3DS-family system into a pocket companion for coding agents running on a developer-controlled computer.

It is not trying to squeeze an IDE or a language model onto the 3DS. It uses the 3DS as a purpose-built **capture + control + monitoring device**.

The ideal experience is:

> Leave the desk, pull out the 3DS, hold a button and speak a request, watch the agent work, and handle the one decision that actually needs you.

## 2. Target user

**Accepted**

The primary user is a developer who:

- owns a modded Nintendo 3DS-family system;
- already uses coding agents or agentic developer tools;
- is comfortable installing homebrew and a desktop companion;
- values a weird, focused handheld interface more than mainstream convenience.

This is intentionally niche.

## 3. Product principles

### 3.1 Capture first

Text, voice, photo, and stylus drawing are all forms of a single concept: a **Capture**.

The system should normalize these inputs before handing them to an agent.

### 3.2 Agent agnostic

3gent must not be coupled to one vendor or CLI.

Agent-specific logic lives in desktop-side adapters.

Initial/future adapters may include:
- Codex
- Claude Code
- Herdr-managed agents
- other coding agents with usable local interfaces

### 3.3 Thin 3DS client

The 3DS owns:
- UI
- buttons/touch/stylus
- microphone capture
- camera capture
- software keyboard
- local device state
- pairing UX
- network transport to the bridge/relay
- rendering responses and approval requests

The 3DS does **not** own:
- model inference
- provider authentication
- repository access
- shell execution
- transcription
- agent process management
- final security policy

### 3.4 Voice first, not voice only

Voice is the primary interaction because typing on the 3DS is slower.

Text remains a first-class fallback through the built-in software keyboard.

### 3.5 Design for the actual device

Use:
- physical buttons for fast actions;
- the top screen for readable output and status;
- the bottom screen for controls and touch;
- the stylus for capture/drawing rather than desktop-style precision UI.

Do not pretend the 3DS is a tiny laptop.

### 3.6 Safe remote control

Approvals must be:
- understandable;
- difficult to trigger accidentally;
- scoped to a session;
- enforced by the desktop bridge even if the client behaves incorrectly.

### 3.7 Bidirectional companion

3gent is not merely an input device or prompt sender. The user must be able to
observe and steer the remote agent from the handheld.

Every production connection mode must support both directions:

- 3DS to bridge: captures, approval responses, interrupts, and session controls;
- bridge to 3DS: agent state, streamed responses, progress, approval requests,
  errors, and completion.

If the user must return to the laptop to understand what the agent is doing, the
core product loop is incomplete.

For the functional-core release, visible progress means normalized working/
waiting/idle state, streamed assistant responses, bounded diff summaries,
approval requests, errors, and completion. Detailed per-tool timelines and raw
command output are not required for this small-screen release and remain a later
UI decision; they must not replace the streamed response itself.

## 4. Core use cases

### UC-01 — Voice prompt

Target flow (implemented in the current hardware-test build; the fake adapter
uses a deterministic mock transcription backend but the same review controls):

1. User opens a connected agent session.
2. User holds push-to-talk.
3. 3DS records a bounded audio capture.
4. User releases the button.
5. Capture is uploaded.
6. Desktop side transcribes it.
7. 3DS shows the transcript for review.
8. User explicitly sends it, edits it through the native keyboard, or cancels.
9. The accepted transcript becomes an agent prompt.
10. Agent response streams back.
11. 3DS renders progress and final output.

### UC-02 — Typed prompt

1. User selects Type.
2. Native 3DS software keyboard opens.
3. Text is returned to 3gent.
4. Text is sent through the same Capture pipeline.
5. Response appears on the 3DS.

### UC-03 — Approval

1. Agent requests permission.
2. 3gent shows what is being requested.
3. User can inspect details.
4. User approves, declines, or cancels.
5. Desktop bridge applies its own policy and forwards an allowed response.

### UC-04 — Remote connection

User can interact with their agent while the 3DS and development computer are on different networks.

### UC-05 — Photo capture (after the core MVP)

User takes a photo and attaches it to a prompt.

Examples:
- error shown on another display;
- handwritten note;
- hardware/setup photo;
- whiteboard.

### UC-06 — Stylus sketch (outside the current product goal)

User draws a diagram or wireframe and sends it as a Capture.

The Capture model keeps this possible, but the current build goal deliberately
omits the sketch canvas.

## 5. MVP

### Required

- Pair 3DS and desktop bridge.
- QR pairing as the default UX.
- Manual pairing fallback.
- Select/resume a session.
- Push-to-talk recording.
- Upload audio.
- Off-device transcription.
- Typed prompt through native software keyboard.
- Camera photo capture and next-prompt attachment.
- Stream/display agent output.
- Show agent state.
- Handle basic approvals.
- Interrupt/cancel an agent turn.
- Local LAN path.
- Remote path.
- Compatible hotspot workflow.

### Explicitly deferred

- full terminal emulation;
- drawing canvas;
- handwriting recognition;
- rich diff editor;
- multi-host orchestration;
- polished public relay;
- auto-configuring hotspots on every OS;
- background service complexity that is not required for the core loop.

## 6. MVP success criterion

The MVP succeeds when:

> A developer can leave their computer, connect the 3DS from another network, speak a request to an existing coding-agent environment, receive useful streamed output, and safely respond to an approval request without returning to the computer.

## 7. Product non-goals

3gent is not:
- a replacement IDE;
- a new coding-agent harness;
- a language model running on the 3DS;
- unrestricted remote shell access by default;
- a requirement to use one AI provider;
- a terminal-emulator project disguised as an agent remote.

## 8. Open product decisions

See `DECISIONS.md`.

Important unresolved items:
- licensing;
- whether Old 3DS remains fully supported after benchmarking;
- exact approval categories;
- production remote transport and reconnect behavior.
- whether production voice shares the control connection or uses an independent
  media connection.
