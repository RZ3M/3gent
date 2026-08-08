# 3gent — Technical Architecture

**Status:** Draft v0.2

## 1. System overview

```text
┌─────────────────────┐
│ Nintendo 3DS client │
│ capture + control   │
└─────────┬───────────┘
          ⇅ local or remote 3gent protocol
┌─────────▼───────────┐
│ Desktop bridge      │
│ security + adapters │
└──────┬───────┬──────┘
       │       │
       │       ├──────────────► transcription / media handling
       │
       ├──────────────► Codex adapter
       ├──────────────► Herdr adapter
       ├──────────────► Claude adapter
       └──────────────► future adapters

Optional remote topology:

3DS ⇄ relay ⇄ desktop bridge ⇄ local agent/runtime
```

### Bidirectional contract

The arrows are semantically bidirectional, not just request/acknowledgement:

```text
3DS → bridge: Capture, interrupt, approval response, session command
3DS ← bridge: agent state, assistant output, progress, approval request, error
```

The desktop bridge translates agent-native events into versioned 3gent events.
The client renders those events incrementally so the user can watch work and
respond without returning to the laptop.

## 2. Nintendo 3DS client

### Recommended base stack

- current devkitPro toolchain;
- devkitARM;
- libctru;
- citro2d where useful for lightweight 2D rendering;
- C or C++.

Use the official devkitPro `3ds-examples` repository as the first reference for:
- input;
- software keyboard;
- networking;
- audio;
- camera;
- graphics.

### Responsibilities

- application lifecycle;
- screen rendering;
- button/touch/stylus input;
- native software keyboard invocation;
- bounded microphone recording;
- camera capture later;
- local Capture queue;
- connection state;
- pairing UX;
- session list/state;
- receiving structured events;
- rendering streamed text;
- rendering approval requests.

### Runtime I/O invariant

Once the interactive frame loop starts, network setup, send, receive, and audio
finalization must make bounded zero-wait progress. A half-open connection must
not prevent screen drawing, button scanning, or local navigation.

The `0.1.2-stage1.5` client implements this as a single-threaded per-frame
network pump with fixed control-frame, command, response, and audio queues.
Push connect/send/receive, heartbeat, and reconnect all follow the invariant.
Startup audio warmup remains a separately visible, bounded development step.
DNS and TLS must preserve the same behavior.

### Constraints

Assume:
- limited memory;
- slow CPU compared with modern phones;
- small screens;
- 2.4 GHz 802.11b/g Wi-Fi;
- public/guest Wi-Fi can be inconsistent;
- network switches, sleep, and reconnects need explicit testing.

Avoid:
- heavy web stacks;
- embedded browser UX;
- large dependency trees;
- desktop terminal emulation in the MVP.

## 3. Desktop bridge

The bridge is 3gent's trusted local control plane.

### Initial implementation

Stage 1 implements the bridge in TypeScript on Node.js. The fake-agent adapter,
HTTP transport, protocol validation, event store, and command registry are
separate modules so the Codex adapter can replace only the fake-agent boundary
in Stage 2. Version `0.1.2-stage1.5` adds a separate development-only raw TCP
control link. Text captures, interrupts, and approval responses travel toward
the bridge while acknowledgements, errors, agent state, and response events are
pushed back as bounded JSON lines. The link preserves the per-session cursor,
replays after reconnect, retries one unacknowledged command by ID, sends
heartbeats, and reconnects with jitter. Microphone PCM continues over the proven
independent HTTP stream. Every runtime operation advances asynchronously in the
handheld frame loop. This local framing experiment is not the final encrypted
remote transport.

### Responsibilities

- agent process/session integration;
- repository context;
- media conversion;
- speech transcription;
- pairing credential issuance;
- policy enforcement;
- local session registry;
- translating agent-native events to the 3gent protocol;
- reconnect/replay logic;
- relay connection;
- secrets.

### Security boundary

The bridge is authoritative.

A 3DS request is an input to policy, not an instruction that bypasses policy.

For example:

```text
3DS says "approve"
        ↓
bridge verifies:
- request still exists
- request belongs to session
- nonce/event id matches
- approval has not expired
- action is remotely approvable
        ↓
agent-native approval response
```

## 4. Agent adapter layer

3gent uses an internal adapter interface.

Conceptual API:

```text
list_sessions()
start_session()          # if/when product decision allows
resume_session()
send_capture()
interrupt_turn()
subscribe_events()
respond_to_approval()
get_session_state()
```

Normalized state:

```text
offline
idle
working
waiting_for_user
completed
failed
```

Normalized events may include:

```text
session.started
session.updated
turn.started
turn.completed
assistant.text.delta
assistant.text.completed
agent.status.changed
approval.requested
approval.resolved
diff.updated
capture.upload.progress
session.error
```

## 5. Codex adapter

### Recommended integration

Use the official local **Codex app-server** as the primary rich integration surface.

Why:
- it exposes conversation/thread lifecycle;
- streams turn/item events;
- exposes approvals;
- can surface updated diffs;
- avoids scraping terminal output.

### Transport choice

The bridge should initially spawn/connect to:

```text
codex app-server --listen stdio://
```

and speak the app-server's local JSON-RPC/JSONL protocol.

At implementation time:
- generate or inspect the schema produced by the user's installed Codex version;
- do not blindly vendor an old protocol snapshot;
- translate all Codex-specific objects at the adapter boundary.

Do **not** make the 3DS speak Codex app-server directly.

Do **not** depend on Codex's direct WebSocket listener for production while it is documented as experimental/unsupported.

### Mapping example

```text
Codex thread       → 3gent session
Codex turn         → 3gent turn
agent message delta→ assistant.text.delta
approval request   → approval.requested
turn diff update   → diff.updated
```

## 6. Herdr adapter

Herdr is useful as an optional runtime/multiplexer integration.

It can:
- preserve real terminal panes;
- identify supported agents;
- report lifecycle states such as working/blocked/idle;
- send prompts/keys;
- read recent agent output.

3gent should integrate with Herdr through its supported control surfaces rather than reimplementing a terminal multiplexer.

Herdr is optional; it must not be required for the base product.

## 7. Relay

### Purpose

Allow the 3DS and bridge to find each other when they are not on the same LAN.

### Relay responsibilities

- authenticate paired endpoints;
- route messages;
- apply rate/size limits;
- handle reconnect;
- avoid holding agent/provider credentials;
- minimize retained content.

### Non-responsibilities

The relay should not:
- own the repository;
- run the coding agent by default;
- receive provider API keys;
- become the desktop bridge.

### Hosting decision

The first relay is self-hosted. The application protocol should remain usable
by a later hosted service, but a public 3gent relay is not part of the first
functional product.

## 8. Connection modes

### Mode A — Direct LAN

```text
3DS ⇄ Wi-Fi LAN ⇄ desktop bridge
```

Best for development.

### Mode B — Compatible hotspot

```text
3DS ⇄ 2.4 GHz hotspot ⇄ phone/laptop ⇄ internet
```

This is a first-class user workflow because 3DS Wi-Fi support is old and guest/captive networks are not guaranteed to be friendly.

### Mode C — Remote relay

```text
3DS ⇄ internet ⇄ relay ⇄ internet ⇄ desktop bridge
```

Core remote-use path.

The first relay distribution is self-hosted. A hosted service can be added
later without changing the 3DS application protocol.

### Remote transport sequencing

Remote control must ultimately use encrypted, push-capable, bidirectional
delivery. WSS leads as the remote candidate because hosted relays, reverse
proxies, and ordinary web infrastructure are real product constraints. A small
raw TLS protocol remains the fallback when self-hosting or measured handheld
cost makes it preferable.

That framing choice is deliberately downstream of R-010. TLS library support,
Old 3DS handshake time and memory, DNS without blocking the frame loop,
certificate validation with an unreliable user-set clock, session resumption,
and reconnection are the expensive shared risks. Push framing should not hide or
lead those measurements.

Before secure remote transport, the local slice should prove pushed events,
heartbeat/liveness, reconnect with jitter, and cursor resume. Sequence numbers,
command IDs, replay, and deduplication remain application-protocol properties
across either WSS or raw TLS.

## 9. QR pairing

Default pairing UX:

1. Bridge displays QR code.
2. 3DS camera scans it.
3. QR contains only short-lived bootstrap information.
4. 3DS contacts the bridge/relay.
5. Both sides complete a pairing exchange.
6. Bridge issues revocable device credentials.
7. Pairing is saved.

Example conceptual QR payload:

```text
3gent://pair?v=1&endpoint=...&nonce=...&fingerprint=...
```

The exact wire representation is not locked.

Rules:
- no permanent auth secret in QR;
- short expiration;
- replay resistance;
- manual code fallback;
- camera/QR decoding library must be evaluated on hardware.

## 10. Capture pipeline

All input becomes a Capture:

```text
Capture {
  id
  kind: text | audio | photo | sketch
  session_id
  created_at
  optional_text
  media_metadata
  media_reference
}
```

This is a conceptual model, not a frozen serialization schema.

### Voice

```text
mic → bounded capture/stream → desktop media handling → transcribe → prompt → agent
```

Whether remote production voice transport streams during capture or uploads
after release remains a measured transport decision. The proven local path uses
fixed client buffers, a live chunked request, and laptop-side WAV assembly.

### Text

```text
software keyboard → text capture → agent
```

### Photo

```text
camera → image capture → optional resize → upload → agent
```

### Sketch

```text
stylus strokes → bitmap/vector representation → upload → agent
```

## 11. Recommended initial repo boundaries

```text
client-3ds/
  source/
  include/
  assets/

bridge/
  src/
  adapters/

relay/
  src/

protocol/
  schemas/

tools/
  dev-server/
  fixtures/
```

Create a boundary only when its stage needs it; `relay/` and production adapter
directories remain deferred.
