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

The current priority is **Stage 0 feasibility**.

Prove these independently:

1. Basic 3DS app builds and launches.
2. Native software keyboard returns text.
3. Local networking can send a small request and receive a response.
4. Response text can be rendered incrementally.
5. Microphone audio can be captured into a bounded buffer/file.
6. QR scanning feasibility can be tested later without blocking the first network loop.

Do not build the relay, production auth, or multiple agent adapters before these are proven.

## 3DS implementation expectations

- Use the current devkitPro-supported toolchain.
- Prefer libctru and official devkitPro examples as primary references.
- Prefer simple C/C++ and small dependencies.
- Keep allocations bounded and obvious.
- Treat network, audio, camera, and sleep/resume behavior as unreliable until tested on hardware.
- Do not silently depend on New 3DS-only behavior unless documented.
- Keep Old 3DS compatibility as the default target until a benchmark proves it impractical.

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

## Definition of done for Stage 0 changes

A Stage 0 spike is done when:

- It builds reproducibly.
- The hardware test procedure is written down.
- Success/failure is observable on screen or in logs.
- No unrelated architecture has been added.
- The result is recorded in `docs/RESEARCH.md`.
