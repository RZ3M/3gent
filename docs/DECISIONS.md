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

### D-013 — TypeScript/Node desktop bridge

**Status:** Accepted

The first production-shaped desktop bridge uses TypeScript on a supported Node.js
LTS release. It keeps Stage 1 iteration fast, fits the JSON/event-oriented
protocol, and provides a straightforward subprocess and JSON-RPC boundary for
agent adapters. Agent-specific objects still stop at the adapter boundary.

This does not require the relay or every future component to use TypeScript.

---

## Proposed

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

### D-P08 — Voice capture transport

**Status:** Proposed / Stage 0 experiment

Candidate A: buffer a complete bounded capture on the 3DS, then upload.
- tolerates a temporarily unavailable connection after recording;
- simple file-oriented retry;
- recording duration consumes scarce handheld memory or SD-card I/O.

Candidate B: stream PCM while push-to-talk is held, then finalize media on the
desktop.
- constant and small 3DS memory use;
- removes handheld RAM as the practical duration limit;
- exposes connection stalls during capture and requires clean partial-stream
  handling.

Stage 0 uses Candidate B over a disposable HTTP chunked endpoint. Keep the
production decision Proposed until physical 3DS, hotspot, reconnect, and remote
transport behavior are measured.

### D-P09 — Persistent client transport

**Status:** Proposed / Stage 0 experiment

Candidate A: open a connection for each action.
- simplest failure isolation;
- physical testing measured user-visible setup latency of roughly one to two
  seconds before small LAN requests or microphone streaming began.

Candidate B: maintain persistent connections while the app is active.
- removes repeated handshake latency and better matches interactive agent use;
- requires framing, idle-connection detection, reconnection, and separate media
  and control flow handling.

Version `0.0.8-stage0` tested Candidate B using two warm HTTP/1.1 connections and
`Content-Length` response framing. The physical retest passed and the user
reported that text and audio startup felt responsive. Stage 1 therefore keeps
the two warm local connections.

Version `0.1.1-stage1` removes runtime socket waits from the interactive frame
loop. A bounded state machine advances control and audio work once per frame;
user commands can cancel an in-flight background event read. This is a required
property of any later transport, but it still does not accept the production
remote transport or the final number of connections.

### D-P10 — Stage 1 event delivery

**Status:** Proposed / local vertical-slice experiment

Stage 1 uses persistent HTTP/1.1 for bounded commands and cursor-based polling
for newline-delimited JSON event batches. Per-session sequence numbers allow a
client to request events after its last applied cursor and replay a bounded
history after a short disconnect.

Version `0.1.1-stage1` makes those checks asynchronous and adaptive: about 100 ms
while working, 250 ms while waiting for approval, one second while idle, and an
exponential one-to-ten-second failure retry. This reduces idle log and network
volume without sacrificing the fake agent's current response cadence.

This choice reuses the low-latency socket behavior proven on physical 3DS
hardware and preserves cursor replay while the fake agent runs. Polling remains
a temporary local mechanism: its bounded batch/2 KiB response ceiling cannot be
assumed to sustain real adapter event rates. Pushed events are the next local
transport experiment.

### D-P11 — Secure bidirectional remote transport

**Status:** Proposed / resequenced behind R-010

WSS is the leading remote control candidate because relay hosting, proxy
traversal, and standard web infrastructure are real product requirements. A
small raw TLS protocol is the fallback, especially for a self-hosted topology.

Do not accept either framing until R-010 measures the shared TLS cost on Old 3DS:

- maintained library/toolchain path;
- handshake time, working memory, and reconnect/resumption behavior;
- non-blocking hostname resolution;
- certificate validation when the handheld RTC is incorrect;
- viable endpoint identity and credential rotation.

The WebSocket framing layer is small and reversible compared with TLS. The
current devkitPro package situation must be reconfirmed during R-010 rather than
assuming packaged WebSocket support. Separately decide whether control and
audio share one secure connection or use independent connections after measuring
backpressure, failure isolation, and the cost of a second TLS handshake.
