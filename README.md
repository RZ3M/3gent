# 3gent

**3gent** is an open-source Nintendo 3DS companion for remote coding agents.

The 3DS is a **thin client**: it captures voice, text, and photos; shows agent
state and responses; and lets the user handle approvals. A desktop-side bridge
owns the coding environment, agent integrations, transcription, repository
access, and security-sensitive operations. Stylus sketch capture is deliberately
outside the current product goal.

## Product sentence

> A pocket remote for your coding agents, built around the hardware the 3DS is actually good at: microphone, camera, buttons, and two small screens.

## What we are building now

Stage 0 proved the risky handheld boundaries on physical hardware: the homebrew
app launches, the native keyboard returns text, local networking works,
incremental output renders and scrolls, and microphone PCM streams using bounded
client memory. Two warm HTTP connections removed the measured one-to-two-second
fresh-connection delay; the user confirmed the revised text and audio paths feel
responsive.

Stage 1 now turns those isolated proofs into a local fake-agent vertical slice:

```text
captures + commands: 3DS → TypeScript bridge → agent
state + responses:  3DS ← TypeScript bridge ← agent
```

It exercises session state, streamed text deltas, interruption, approval
requests and responses, command deduplication, and cursor-based event replay
before a real coding-agent adapter is introduced. 3gent is a bidirectional
agent companion: sending input without showing what the agent is doing is not a
complete product loop.

## Implementation status

The Stage 0A-E implementation now lives in `client-3ds/` and
`tools/dev-server/`. It includes:

- a minimal top/bottom-screen 3DS application;
- native software keyboard input;
- a bounded, timeout-controlled local HTTP request;
- visible connecting, success, cancellation, and error states;
- incremental response rendering with bounded scrollback and held-button navigation;
- bounded-memory push-to-talk microphone streaming and laptop-side WAV output;
- a standard-library development server with automated tests and inspectable
  audio output.

The keyboard, LAN echo loop, incremental rendering, and scroll navigation have
passed on physical 3DS hardware. The development server tests pass on the host,
the 3DS application builds with devkitARM r68 and libctru 2.7.0, and the same
build passes in CI using devkitPro's official toolchain container. Held-button
scroll repeat also passed its focused hardware retest. Version `0.0.7-stage0`
fixed the observed 40 ms microphone stall on hardware. Version `0.0.8-stage0`
warms and reuses separate low-latency command and audio connections so user
actions no longer pay a fresh TCP handshake; its focused hardware retest passed.

The new `bridge/` service is a TypeScript/Node implementation of the Stage 1
agent boundary with a deterministic fake adapter. Its automated tests cover the
protocol version, sessions, ordered/replayed events, duplicate commands,
approvals, interruption, and streamed audio/WAV output. The
`0.1.2-stage1.5` 3DS client now uses a persistent pushed control link: commands
go to the bridge and agent events return immediately without HTTP polling. It
adds heartbeat, jittered reconnect, cursor replay, visible resync, and safe
retry of one unacknowledged command. The bridge has 23 passing automated tests
and the handheld build succeeds; this new pushed path still needs its focused
physical hardware check.

See [`client-3ds/README.md`](client-3ds/README.md) for build instructions and the
physical Stage 1 checklist. See [`bridge/README.md`](bridge/README.md) for the
current local bridge. The Python server under `tools/dev-server/` remains as the
reproducible Stage 0 fixture.

## Core product decisions

- Open source.
- Target audience: developers with modded 3DS-family systems.
- Agent-agnostic architecture.
- The 3DS is a thin client, not an AI runtime.
- Voice is the primary input.
- Native software keyboard is the text fallback.
- Photo capture follows the core MVP; stylus sketch capture is deferred outside
  the current goal.
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
├── bridge/              # TypeScript desktop bridge and fake adapter
├── protocol/            # machine-readable protocol schemas
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

**Stage 1.5: pushed local fake-agent transport.**

The Stage 1 fake-agent loop has a qualitative physical pass. Local pushed
bidirectional control is now implemented and host-tested with no event polling;
its physical reconnect, heartbeat, audio concurrency, sleep/resume, and
no-freeze checklist is next. The installed Codex app-server schema has been
inspected in preparation for the first real adapter. Secure-transport
feasibility still precedes the self-hosted remote relay.

## License status

A project license has not been selected yet. Public repository visibility does
not grant reuse or redistribution rights by itself. See D-P06 in
[`docs/DECISIONS.md`](docs/DECISIONS.md); a license must be chosen before the
project is treated as an open-source release.
