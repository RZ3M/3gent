# AGENTS.md — instructions for coding agents

You are working on **3gent**, an open-source Nintendo 3DS remote for coding agents.

## Before changing code

Read, in this order:

1. `README.md`
2. `docs/PRODUCT.md`
3. `docs/ARCHITECTURE.md`
4. `docs/DECISIONS.md`
5. `docs/ROADMAP.md`
6. The specific docs relevant to the task.

The `docs/` directory is the system of record. This file is a map, not an encyclopedia.

## Product invariants

Do not violate these without an explicit architecture decision:

- The 3DS is a thin client.
- The desktop bridge owns agent integrations and repository access.
- The architecture is agent-agnostic.
- Voice is the primary input; text is a fallback.
- Camera and stylus are future capture modes using the same capture abstraction.
- Remote access is a core requirement.
- QR pairing is the default pairing UX.
- No AI provider credential should be stored on the 3DS.
- Do not implement a new coding-agent harness.
- Do not turn the MVP into a general-purpose terminal emulator.
- Do not invent custom cryptography.

## Current development phase

The current priority is **Stage 1: local fake-agent vertical slice**. The Stage 0
keyboard, LAN, incremental-rendering, scrolling, microphone, and low-latency
connection proofs have passed on physical hardware.

Prove this end-to-end on the 3DS:

1. Text and voice become protocol-versioned Captures.
2. The TypeScript bridge owns the session and fake-agent adapter.
3. Ordered text deltas and state events render through a reconnectable cursor.
4. An active turn can be interrupted.
5. A structured fake approval can be approved or declined.
6. Client memory and protocol payloads remain bounded.

Do not build the relay, production auth, or a real agent adapter until the Stage
1 hardware checklist passes.

## 3DS implementation expectations

- Use the current devkitPro-supported toolchain.
- Prefer libctru and official devkitPro examples as primary references.
- Prefer simple C/C++ and small dependencies.
- Keep allocations bounded and obvious.
- Treat network, audio, camera, and sleep/resume behavior as unreliable until tested on hardware.
- Do not silently depend on New 3DS-only behavior unless documented.
- Keep Old 3DS compatibility as the default target until a benchmark proves it impractical.

## UI changes

`client-3ds/source/ui.c` renders both screens and has no side effects; it reads
only a `UiModel`. Any task that touches it — layout, color, new screen state,
new card — must be validated with the host preview before it is called done:

```sh
make -C tools/ui-preview run
open tools/ui-preview/out/index.html
```

This compiles the real `ui.c` unmodified, so it cannot drift from the device.
Check every affected state, not just the one you changed; a shared helper
(`ui_panel_outlined`, `ui_chip`, wrapping, etc.) can silently break other
screens. Note in your report which states you reviewed and what you looked for.
It cannot prove text fit on a physical panel, camera orientation, frame rate, or
battery behavior — those still need hardware and belong in
`docs/HARDWARE_TEST_CHECKLIST.md` section B.

## Architecture-change policy

If a task requires a decision not already accepted:

1. Add it to `docs/DECISIONS.md` as **Proposed**.
2. Explain alternatives and tradeoffs.
3. Continue only with a reversible implementation if possible.
4. Do not silently mark major product decisions Accepted.

## Codex integration policy

For the eventual Codex adapter, prefer the official local `codex app-server` interface rather than scraping terminal pixels.

Important:
- Prefer the documented local stdio transport.
- Treat direct app-server WebSocket listening as experimental unless official docs say otherwise at implementation time.
- Generate or inspect the app-server schema for the installed Codex version rather than hard-coding an old schema.
- Translate Codex-specific events into 3gent's internal protocol at the desktop bridge boundary.

## Security expectations

- Never commit secrets or tokens.
- No permanent credential in a pairing QR code.
- Pairing bootstrap data must expire.
- Desktop policy is authoritative for approvals.
- A compromised 3DS must not automatically imply unrestricted shell access.
- Never solve TLS/platform problems by inventing a home-grown cipher.

## Work style

For each implementation task:

1. State what you are proving.
2. Keep the change as small as practical.
3. Run available build/tests.
4. Document hardware-only verification steps separately.
5. Report uncertainties instead of guessing.
6. Update `docs/RESEARCH.md` when a feasibility question is answered.
7. Update `docs/DECISIONS.md` when a design decision is actually made.

## Definition of done for current-stage changes

A current-stage slice is done when:

- It builds reproducibly.
- The hardware test procedure is written down.
- Success/failure is observable on screen or in logs.
- No unrelated architecture has been added.
- The result is recorded in `docs/RESEARCH.md`.
- If it touched `client-3ds/source/ui.c`, the affected states were checked with
  `tools/ui-preview` (see "UI changes" above).

## Git commits

Do not add yourself as a co-author, contributor, or "Generated by" byline on
any commit.