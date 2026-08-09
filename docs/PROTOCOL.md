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

## 4. Stage 1 HTTP fixture

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
Polling is not an input event from the user. It was the Stage 1 development
client asking whether the bridge had agent-to-3DS events available. The HTTP
routes remain as a tested fixture and audio path, but `0.1.2-stage1.5` no longer
uses event polling or HTTP for text/approval/interrupt control.

## 3.5 Pairing and device credentials

Pairing is how a handheld learns which machine to talk to and earns the right to
talk to it (D-010, D-022, ADR-0001). It runs before any session exists.

### QR and manual bootstrap

The bridge prints a bootstrap. It is short-lived and carries no credential:

```text
3gent://pair?v=1&h=<host>&p=<httpPort>&q=<pushPort>&c=<code>&n=<bridgeName>
```

| Field | Meaning |
| --- | --- |
| `v` | payload version; a client must refuse a version it does not know |
| `h` | address the handheld should dial, not the bridge's bind address |
| `p` | HTTP port |
| `q` | pushed-control port |
| `c` | one-time pairing code |
| `n` | display name, bounded to 20 characters |

Field names are single letters and the name is bounded because the payload has
to stay inside a QR version a 400×240 camera can resolve; the current payload
lands at version 5–7. `f` is reserved for the endpoint identity fingerprint that
secure transport will need (D-P11) and is not emitted yet.

The manual fallback is the same information as four whitespace-separated fields,
which is what the bridge prints beside the QR code:

```text
192.168.1.42 8080 8081 K7M2-QX4T-9BWF
```

Codes are 12 characters from `23456789ABCDEFGHJKMNPQRSTVWXYZ`, an alphabet with
no visually ambiguous pairs. Grouping dashes are cosmetic; a client normalises
by uppercasing and dropping everything outside the alphabet. Separators may be
spaces, colons or commas.

### Exchange

```text
POST /v1/pair
X-3gent-Protocol-Version: 1
Content-Type: application/json

{"code": "K7M2QX4T9BWF", "deviceName": "New 3DS XL"}
```

```text
201 Created

{
  "protocolVersion": 1,
  "deviceId": "dev_5f2ac91b0e77d4a3",
  "deviceToken": "<opaque base64url string>",
  "deviceName": "New 3DS XL",
  "bridgeName": "jm-mbp",
  "endpoint": {"host": "192.168.1.42", "httpPort": 8080, "pushPort": 8081}
}
```

The endpoint is echoed so a mistyped manual entry is corrected by the bridge
rather than persisted by the handheld.

Rules:

- one offer is redeemable at a time, and redeeming consumes it;
- offers expire (180 s by default, `--pairing-ttl-seconds`);
- the bridge stores only a SHA-256 hash of the token;
- `POST /v1/pair` is the one `/v1/` route that never requires a token, because
  it is how a token is obtained;
- failures are ordinary protocol errors: `PAIRING_NOT_OPEN` (409),
  `PAIRING_CODE_REJECTED` (403), `PAIRING_UNSUPPORTED` (501).

### Presenting the credential

```text
Authorization: Bearer <deviceToken>
```

on every HTTP request, and on the pushed-control link as a `deviceToken` field
in `connection.hello`. One rule covers both transports.

Enforcement is opt-in (`--require-pairing`). With it off the bridge accepts and
records tokens but does not demand them, which keeps the existing plaintext
development path working. See `docs/SECURITY.md` §4 for why defaulting it on
would overstate what a bearer token achieves over an unencrypted link.

Revocation is a bridge-side operation: `--list-devices` and
`--revoke-device <id>`. It takes effect on a running bridge without a restart —
the device store is re-read when the file changes. Deleting the handheld's saved
key is a separate, local-only action and does not revoke anything.

## 4.1 Stage 1.5 local pushed control

The current 3DS build opens a separate development-only raw TCP connection on
port 8081. Both sides set `TCP_NODELAY`. Each frame is one UTF-8 JSON object plus
`\n`, with a hard 4 KiB frame/input limit. The existing event envelope remains
the application payload; the transport adds only small wrappers.

The client starts or resumes with its last successfully applied cursor, and
presents its device credential if it has one (§3.5):

```json
{"protocolVersion":1,"type":"connection.hello","sessionId":"ses_fake_local","after":12,"deviceToken":"..."}
```

The bridge replies with a session snapshot and then pushes every retained event
after that cursor:

```json
{"protocolVersion":1,"type":"connection.ready","session":{"sessionId":"ses_fake_local","state":"idle"},"lastSequence":12}
{"protocolVersion":1,"type":"event","event":{"protocolVersion":1,"eventId":"evt_...","sequence":13,"sessionId":"ses_fake_local","type":"assistant.text.delta","createdAt":"...","payload":{"text":"Hello"}}}
```

Text, interruption, and approval commands share the connection:

```json
{"protocolVersion":1,"type":"command","commandId":"cmd_...","command":{"type":"capture.text","text":"run tests"}}
{"protocolVersion":1,"type":"command","commandId":"cmd_...","command":{"type":"turn.interrupt"}}
{"protocolVersion":1,"type":"command","commandId":"cmd_...","command":{"type":"approval.respond","approvalId":"apr_...","choice":"approve_once"}}
```

The bridge returns `command.ack` with the normal acknowledgement. The 3DS keeps
at most one mutating command and retries the exact frame/ID after reconnect until
it receives that acknowledgement. Bridge-side deduplication prevents a replayed
accepted command from executing twice.

After three seconds without inbound traffic, the 3DS sends a `ping`; the bridge
returns `pong`. Eight seconds without any inbound frame causes the 3DS to close
and reconnect. Retry delay starts at roughly 250 ms, doubles to a ten-second cap,
and includes ±20 percent jitter. The bridge releases clients that send nothing
for twelve seconds.

If a cursor is expired or ahead after a bridge restart, the bridge sends
`resync.required` with the current bounded session snapshot and closes the link.
The 3DS visibly marks the output gap, applies the snapshot cursor, and reconnects.
The event store retains at most 256 events and 128 KiB per session. Slow peers
are handled by socket backpressure and cursor replay rather than an unbounded
per-client queue.

This raw TCP framing proves local push mechanics only. It is unauthenticated and
unencrypted, must not be exposed to the internet, and does not settle WSS versus
raw TLS for the self-hosted remote relay.

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

An empty poll has a zero-byte body. The bridge retains at most 256 events and
128 KiB per session. If `after` is older than retained history, it returns
`EVENT_CURSOR_EXPIRED`. If it is newer than the latest event—for example, after
an in-memory bridge restart—it returns `EVENT_CURSOR_AHEAD`. The client then
fetches the current session snapshot, advances to its `lastSequence`, and
visibly reports that skipped history cannot be reconstructed.

## 6. Stage 1 commands

```text
GET  /health
GET  /v1/sessions?limit=...&cursor=...
POST /v1/sessions
GET  /v1/sessions/:sessionId
POST /v1/sessions/:sessionId/resume
GET  /v1/events?sessionId=...&after=...&limit=...
POST /v1/sessions/:sessionId/captures/text
POST /v1/sessions/:sessionId/captures/audio
POST /v1/sessions/:sessionId/captures/photo
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
capture.transcript.delta
capture.transcribed
capture.photo.ready
capture.attached
turn.interrupted
turn.diff.updated
turn.completed
error
```

Normalized session states are:

```text
offline | idle | working | waiting_for_user | completed | failed
```

The Codex adapter maps app-server threads and turns into these same objects.
Codex UUIDs never cross the bridge boundary: deterministic opaque `ses_codex_`
identifiers are used instead. `turn.diff.updated` carries only bounded counts
for files, additions and deletions; raw diffs remain on the trusted laptop.

Photo bodies contain exactly 192,000 bytes of 400×240 RGB565 with content type
`application/x-3gent-rgb565; width=400; height=240`. The bridge writes a BMP and
retains one photo for the session's next successful prompt. `capture.photo.ready`
marks it pending; `capture.attached` marks it consumed.

The hardware-test self-hosted relay does not change application frames. It pairs
one public byte stream with one authenticated outbound bridge tunnel, so cursor,
heartbeat, command-ID and media semantics remain identical. This plaintext
feasibility transport is not protocol-v1 production security.

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
2. physically prove the implemented local pushed events, heartbeat, reconnect
   with jitter, and cursor resume;
3. run R-010 for TLS, DNS, clock/certificate, memory, and handshake measurements;
4. decide framing after those results.

WSS leads for remote use because relay/proxy interoperability matters. A small
raw TLS protocol is the fallback. Neither is Accepted, and the number of secure
connections remains open. Ordinary HTTP polling remains only the local Stage 1
fixture.

Remote transport must be encrypted with a standard, reviewed security design.

Do not invent custom cryptography to work around platform limitations.
