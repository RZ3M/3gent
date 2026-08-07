# 3gent

**3gent** is an open-source Nintendo 3DS companion for remote coding agents.

The 3DS is a **thin client**: it captures voice, text, photos, and stylus sketches; shows agent state and responses; and lets the user handle approvals. A desktop-side bridge owns the coding environment, agent integrations, transcription, repository access, and security-sensitive operations.

## Product sentence

> A pocket remote for your coding agents, built around the hardware the 3DS is actually good at: microphone, camera, stylus, buttons, and two small screens.

## What we are building first

The first milestone is deliberately tiny:

1. Build and launch a 3DS homebrew app.
2. Open the native 3DS software keyboard.
3. Enter a text prompt.
4. Send it over local Wi-Fi to a tiny development server.
5. Receive a response.
6. Display that response incrementally on the 3DS.

After that works, add microphone capture and the real desktop bridge.

## Stage 0 implementation status

The Stage 0A-D implementation now lives in `client-3ds/` and
`tools/dev-server/`. It includes:

- a minimal top/bottom-screen 3DS application;
- native software keyboard input;
- a bounded, timeout-controlled local HTTP request;
- visible connecting, success, cancellation, and error states;
- incremental response rendering with bounded scrollback and held-button navigation;
- a standard-library development echo server with automated tests.

The keyboard, LAN echo loop, incremental rendering, and scroll navigation have
passed on physical 3DS hardware. The development server tests pass on the host,
the 3DS application builds with devkitARM r68 and libctru 2.7.0, and the same
build passes in CI using devkitPro's official toolchain container. Held-button
scroll repeat was added from hardware feedback and still needs its focused retest.

See [`client-3ds/README.md`](client-3ds/README.md) for build instructions and the
physical test checklist. See [`tools/dev-server/README.md`](tools/dev-server/README.md)
for the local server.

## Core product decisions

- Open source.
- Target audience: developers with modded 3DS-family systems.
- Agent-agnostic architecture.
- The 3DS is a thin client, not an AI runtime.
- Voice is the primary input.
- Native software keyboard is the text fallback.
- Photos and stylus sketches are first-class future capture modes.
- Remote use is a core requirement.
- Compatible phone/computer hotspot usage is first-class.
- QR pairing is the default pairing method.
- Manual pairing should remain as a fallback.
- Do not build a new coding-agent harness.
- Do not make the 3DS a full terminal emulator for the MVP.

## Repository map

```text
3gent/
├── AGENTS.md
├── README.md
├── client-3ds/          # 3DS homebrew client
├── bridge/              # desktop bridge (created after feasibility)
├── relay/               # optional remote relay (later)
├── protocol/            # shared protocol schemas/specs
├── tools/               # local development utilities
├── docs/
│   ├── PRODUCT.md
│   ├── ARCHITECTURE.md
│   ├── PROTOCOL.md
│   ├── UI_UX.md
│   ├── SECURITY.md
│   ├── RESEARCH.md
│   ├── ROADMAP.md
│   ├── DECISIONS.md
│   ├── INSPIRATION.md
│   ├── SOURCES.md
│   └── adr/
└── prompts/
    ├── 00_BOOTSTRAP_CODEX.md
    └── 01_NEXT_AFTER_STAGE0.md
```

## Continue development with Codex

1. Clone the repository and open Codex at its root.
2. Let Codex inspect `AGENTS.md` and all docs before writing code.
3. Use the scoped tasks under `prompts/` in order.
4. Keep architecture decisions in `docs/DECISIONS.md` and `docs/adr/`.

Codex supports repository-level `AGENTS.md` instructions, so this repo uses `AGENTS.md` as a concise map and keeps the detailed source of truth under `docs/`.

## Current phase

**Phase 0: feasibility.**

Do not try to build the complete remote-agent product yet. Prove the risky 3DS platform boundaries one at a time.

## License status

A project license has not been selected yet. Public repository visibility does
not grant reuse or redistribution rights by itself. See D-P06 in
[`docs/DECISIONS.md`](docs/DECISIONS.md); a license must be chosen before the
project is treated as an open-source release.
