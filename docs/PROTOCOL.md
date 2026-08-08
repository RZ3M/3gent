# 3gent — Protocol Design Notes

**Status:** Stage 1 protocol v1 draft / intentionally evolvable

The protocol must remain independent of Codex, Claude Code, Herdr, or any other specific runtime.

## 1. Goals

- small enough for a 3DS client;
- versioned;
- reconnectable;
- explicit event IDs;
- bounded payload sizes;
- structured approvals;
- media uploads separated from control messages where useful;
- explicitly bidirectional: captures/control toward the bridge and agent
  state/output/approvals back toward the 3DS;
- straightforward to debug.

## 2. Non-goals

The 3DS protocol is not:
- SSH;
- a raw PTY stream;
- Codex app-server pass-through;
- an AI-provider API;
- a repository synchronization protocol.

## 3. Stage 0 development protocol

For the first networking spike, keep it intentionally disposable.

A valid Stage 0 test can be as small as:

```text
3DS:
POST /echo
Content-Type: text/plain

hello from 3gent

Server:
200 OK
hello from bridge: hello from 3gent
```

The point is to prove:
- socket/HTTP viability;
- request/response;
- timeouts;
- text rendering.

The Stage 0E microphone experiment additionally uses one disposable
`POST /audio/stream` request with HTTP/1.1 chunked transfer encoding. Its body is
signed little-endian mono PCM16 at 16,364 Hz; the development server assembles a
WAV incrementally and commits it only after a clean end marker. This proves
bounded live upload behavior and is not a frozen production media protocol.

After physical testing showed that fresh 3DS TCP setup added roughly one to two
seconds to each action, the Stage 0 client was changed to warm and reuse two
HTTP/1.1 connections: one for control/text requests and one for microphone
streaming. This remains a disposable latency experiment, not a decision to use
HTTP keep-alive as the production remote transport.

Do not turn this disposable endpoint into the production protocol by accident.

## 4. Stage 1 transport

The local vertical slice uses persistent HTTP/1.1 connections:

- bounded request/response commands on the command connection;
- chunked PCM upload on the audio connection;
- short cursor-based event polls when the command connection is idle.

Runtime requests are state machines advanced once per 3DS frame; no runtime
socket operation may wait inside the input/render loop. A user command takes
priority over and may cancel an in-flight event check. Event cadence is adaptive
in `0.1.1-stage1`: approximately 100 ms while working, 250 ms while waiting for
approval, one second while idle, and exponential one-to-ten-second retry after a
failure.

Every Stage 1 request sends:

```text
X-3gent-Protocol-Version: 1
```

Mutating commands also send a bounded unique command ID:

```text
X-3gent-Command-Id: cmd_3ds_000001
```

The bridge remembers a bounded set of command IDs. Repeating an accepted command
ID returns the original acknowledgement without running the command twice.

This is a local transport experiment. Protocol messages are independent from the
transport and the remote path must later add standard authenticated encryption.
Polling is not an input event from the user; it is the current development
client asking whether the bridge has agent-to-3DS events available.

## 5. Envelope

Structured control events have:

```json
{
  "protocolVersion": 1,
  "eventId": "evt_000001",
  "sequence": 1,
  "sessionId": "ses_fake_local",
  "type": "assistant.text.delta",
  "createdAt": "2026-08-07T22:00:00.000Z",
  "payload": {"text": "Hello"}
}
```

Rules:

- `protocolVersion` is required and currently equals `1`;
- `eventId` is globally unique enough for client deduplication;
- `sequence` is strictly increasing within one session;
- `sessionId` routes the event and is never inferred from UI state;
- `type` is an agent-agnostic event name;
- `createdAt` is informational bridge time, not an ordering primitive;
- `payload` is an object whose shape is defined by `type`;
- unknown event types are ignored after advancing the cursor;
- each encoded event line is at most 640 UTF-8 bytes;
- event count per batch is bounded by the requested limit, up to 32.

Event polls return UTF-8 newline-delimited JSON (`application/x-ndjson`). A client
requests only events after the last sequence it has successfully applied:

```text
GET /v1/events?sessionId=ses_fake_local&after=12&limit=8
```

An empty poll has a zero-byte body. The bridge retains 256 events per session in
Stage 1. If `after` is older than retained history, it returns
`EVENT_CURSOR_EXPIRED`. If it is newer than the latest event—for example, after
an in-memory bridge restart—it returns `EVENT_CURSOR_AHEAD`. The client then
fetches the current session snapshot, advances to its `lastSequence`, and
visibly reports that skipped history cannot be reconstructed.

## 6. Stage 1 commands

```text
GET  /health
GET  /v1/sessions
GET  /v1/sessions/:sessionId
GET  /v1/events?sessionId=...&after=...&limit=...
POST /v1/sessions/:sessionId/captures/text
POST /v1/sessions/:sessionId/captures/audio
POST /v1/sessions/:sessionId/turns/current/interrupt
POST /v1/sessions/:sessionId/approvals/:approvalId/respond
```

Text capture bodies are UTF-8 `text/plain` and bounded to 4 KiB. Audio uses the
Stage 0 measured signed PCM16 mono stream at 16,364 Hz and remains bounded to five
minutes. Approval responses use JSON:

```json
{"choice":"approve_once"}
```

Successful mutations return a command acknowledgement:

```json
{
  "protocolVersion": 1,
  "commandId": "cmd_3ds_000001",
  "accepted": true,
  "duplicate": false,
  "sessionId": "ses_fake_local",
  "lastSequence": 3
}
```

## 7. Stage 1 server events

These events are the bridge-to-3DS half of the product protocol. In particular,
`assistant.text.delta` is how the handheld sees the agent's response while it is
being produced; acknowledgements alone are not a substitute.

```text
connection.ready
session.updated
turn.started
assistant.text.delta
assistant.text.completed
approval.requested
approval.resolved
capture.accepted
capture.progress
turn.interrupted
turn.completed
error
```

Normalized session states are:

```text
offline | idle | working | waiting_for_user | completed | failed
```

The fake adapter uses `idle`, `working`, and `waiting_for_user`. Later adapters
may use every state without changing the 3DS protocol.

## 8. Approvals

An approval event needs enough information to render safely without exposing an entire terminal screen.

Conceptually:

```json
{
  "approvalId": "apr_...",
  "sessionId": "ses_...",
  "kind": "command",
  "summary": "Run project tests",
  "details": {
    "command": "...",
    "cwd": "..."
  },
  "choices": ["approve_once", "decline", "cancel"],
  "expiresAt": "..."
}
```

The client never decides which choices are valid by itself. The bridge sends allowed choices.

## 9. Errors

Protocol errors are bounded JSON and never expose a stack trace:

```json
{
  "protocolVersion": 1,
  "error": {
    "code": "SESSION_BUSY",
    "message": "session already has an active turn",
    "retryable": true
  }
}
```

HTTP status still communicates the broad class. The error code is the stable
application-level branch.

## 10. Media

Media should have:
- capture ID;
- kind;
- byte size;
- MIME/format metadata;
- optional duration/dimensions;
- integrity information where appropriate;
- hard limits.

Do not base64 large media inside normal JSON events unless a benchmark shows that it is acceptable.

## 11. Transport decision is pending

Candidates must be tested on actual hardware.

The planned sequence is:

1. keep application-level sequences, command IDs, replay, and deduplication;
2. prove local pushed events, heartbeat, reconnect with jitter, and cursor resume;
3. run R-010 for TLS, DNS, clock/certificate, memory, and handshake measurements;
4. decide framing after those results.

WSS leads for remote use because relay/proxy interoperability matters. A small
raw TLS protocol is the fallback. Neither is Accepted, and the number of secure
connections remains open. Ordinary HTTP polling remains only the local Stage 1
fixture.

Remote transport must be encrypted with a standard, reviewed security design.

Do not invent custom cryptography to work around platform limitations.
