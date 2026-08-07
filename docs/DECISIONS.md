# 3gent — Decision Log

Use this file as the index. Major decisions can get full ADRs in `docs/adr/`.

## Accepted

### D-001 — Open source

**Status:** Accepted

3gent is intended as a public open-source project that others can use and contribute to.

### D-002 — Developer audience

**Status:** Accepted

Target users are developers with modded 3DS-family systems who already understand coding-agent workflows.

### D-003 — Agent-agnostic core

**Status:** Accepted

Agent-specific integrations belong behind desktop-side adapters.

### D-004 — Thin 3DS client

**Status:** Accepted

The 3DS handles capture, display, navigation, and remote control. Heavy processing and repository access remain off-device.

### D-005 — Remote access is core

**Status:** Accepted

3gent is not limited to the same LAN as the development computer.

### D-006 — Hotspot workflow is first-class

**Status:** Accepted

Because the 3DS has older 2.4 GHz Wi-Fi capabilities and public networks are unpredictable, a compatible phone/computer hotspot is a normal connection path.

### D-007 — Voice-first input

**Status:** Accepted

Push-to-talk voice is the primary input experience.

### D-008 — Native software keyboard

**Status:** Accepted

Typed prompts are an MVP fallback using the system software keyboard.

### D-009 — Unified Capture abstraction

**Status:** Accepted

Text, audio, photo, and sketch inputs share one product-level Capture concept.

### D-010 — QR pairing default

**Status:** Accepted

QR code is the default pairing UX. Manual pairing remains a fallback.

### D-011 — No full terminal emulator in MVP

**Status:** Accepted

3gent is agent-first. Optional raw-terminal functionality can be considered later.

### D-012 — Codex adapter should use structured interface

**Status:** Accepted in principle

Prefer Codex app-server through the desktop bridge over terminal scraping.

---

## Proposed

### D-P01 — Desktop bridge language

**Status:** Proposed

Candidate A: TypeScript/Node
- fast iteration;
- strong JSON-RPC/subprocess ecosystem;
- easy web/relay sharing.

Candidate B: Rust
- single binary;
- lower overhead;
- strong long-term systems properties;
- more implementation friction.

Do not lock this merely to complete Stage 0. The Stage 0 echo server can be disposable.

### D-P02 — Old 3DS support

**Status:** Proposed default

Target Old 3DS compatibility unless measured performance makes a feature unreasonable.

Optimize for New 3DS only where explicitly documented.

### D-P03 — Session creation

**Question:**
Can a user start a new coding-agent session from 3gent, or only attach to existing sessions?

### D-P04 — Voice send behavior

**Question:**
On push-to-talk release:
- send immediately;
- show transcript first;
- configurable behavior?

### D-P05 — Relay model

**Question:**
- official hosted relay;
- self-host-only;
- both?

### D-P06 — Project license

**Question:**
Choose a license before public release.

Candidates can be evaluated separately; do not insert a LICENSE file silently.

### D-P07 — Bridge ownership of agent lifetime

**Question:**
Should the bridge launch agents itself, integrate with a multiplexer such as Herdr, attach to existing agent sessions, or support all three via adapters?
