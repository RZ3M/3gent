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

### D-014 — New and resumed agent sessions

**Status:** Accepted

The product supports both starting a new coding-agent session and resuming an
existing one. The first Codex adapter should expose both operations while the
desktop bridge remains authoritative over workspace and policy choices.

### D-015 — Review voice transcripts before sending

**Status:** Accepted

Releasing push-to-talk uploads and transcribes the capture, then shows the
transcript on the 3DS. A separate explicit button sends it to the agent. The
user can cancel and may edit through the native keyboard where practical.
Version `0.3.0-stage3` implements this behavior for both fake and real adapters.

### D-016 — Self-hosted relay first

**Status:** Accepted

The first remote relay is self-hosted. Its protocol must not prevent a later
hosted service, but an official public relay is not required for the first
functional product.

### D-017 — Photo included; stylus sketch deferred

**Status:** Accepted for the current product scope

Photo capture and attachment are included after the core agent loop. Stylus
sketch capture is outside the current goal and may be revisited later.

### D-018 — Codex app-server translation boundary

**Status:** Accepted

The initial Codex adapter spawns the installed `codex app-server` over stdio,
inspects the installed schema, and translates only supported v2 methods/events
at the desktop boundary. Codex thread UUIDs are replaced by deterministic opaque
3gent session IDs. Remote approvals expose only one-shot accept, decline and
cancel; session-wide policy amendments and extended permission profiles remain
desktop-only.

### D-019 — Reverse relay for the self-hosted hardware test

**Status:** Accepted as a reversible test only

The first remote feasibility build uses a small self-hosted reverse TCP relay so
the laptop opens outbound uplinks and the 3DS can reach both HTTP media/session
traffic and pushed control. Bridge uplinks use a bounded token and connection
pool. Per explicit current scope, the 3DS-facing side remains unauthenticated
plaintext and the relay requires `--unsafe-public`; this is not acceptance of a
production transport and must be replaced or secured before release.

---

## Proposed

### D-P02 — Old 3DS support

**Status:** Proposed default

Target Old 3DS compatibility unless measured performance makes a feature unreasonable.

Optimize for New 3DS only where explicitly documented.

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

Version `0.1.2-stage1.5` warms only the independent HTTP audio connection and
opens the pushed control link asynchronously. The legacy HTTP command socket is
retained as fixture/fallback code but is no longer pre-opened by the app.

### D-P10 — Stage 1 event delivery

**Status:** Proposed / local vertical-slice experiment

The historical Stage 1 client used persistent HTTP/1.1 for bounded commands and
cursor-based polling for newline-delimited JSON event batches. Per-session
sequence numbers allow a client to request events after its last applied cursor
and replay a bounded history after a short disconnect.

Version `0.1.1-stage1` makes those checks asynchronous and adaptive: about 100 ms
while working, 250 ms while waiting for approval, one second while idle, and an
exponential one-to-ten-second failure retry. This reduces idle log and network
volume without sacrificing the fake agent's current response cadence.

This choice reuses the low-latency socket behavior proven on physical 3DS
hardware and preserves cursor replay while the fake agent runs. Polling remains
a temporary local mechanism: its bounded batch/2 KiB response ceiling cannot be
assumed to sustain real adapter event rates. Pushed events are the next local
transport experiment.

Version `0.1.2-stage1.5` implements that experiment using a separate
development-only raw TCP control port with bounded newline-delimited JSON. The
3DS sends text captures and controls on the same persistent link that receives
acknowledgements, agent events, errors, and heartbeat replies. It resumes from
the last applied event cursor, retries one unacknowledged command with the same
ID, and reconnects with capped jittered backoff. The proven HTTP audio upload
remains separate.

This local framing is reversible and does not decide D-P11. Host tests pass;
physical reconnect, sleep, Wi-Fi, latency, and no-freeze evidence is pending.

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
