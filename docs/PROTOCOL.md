# 3gent — Protocol Design Notes

**Status:** Pre-v0 / intentionally not frozen

The protocol must remain independent of Codex, Claude Code, Herdr, or any other specific runtime.

## 1. Goals

- small enough for a 3DS client;
- versioned;
- reconnectable;
- explicit event IDs;
- bounded payload sizes;
- structured approvals;
- media uploads separated from control messages where useful;
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

Do not turn this disposable endpoint into the production protocol by accident.

## 4. Event model draft

Later structured control events should have:

```json
{
  "protocolVersion": 1,
  "eventId": "evt_...",
  "sessionId": "ses_...",
  "type": "assistant.text.delta",
  "payload": {}
}
```

Useful properties:
- `eventId` for dedupe;
- `sessionId` for routing;
- protocol version;
- monotonically increasing sequence number per stream/session if replay requires it.

## 5. Draft client commands

```text
session.list
session.open
capture.create
capture.upload
turn.interrupt
approval.respond
ping
```

## 6. Draft server events

```text
connection.ready
session.list
session.updated
turn.started
assistant.text.delta
assistant.text.completed
approval.requested
approval.resolved
diff.updated
capture.accepted
capture.progress
turn.completed
error
```

## 7. Approvals

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

## 8. Media

Media should have:
- capture ID;
- kind;
- byte size;
- MIME/format metadata;
- optional duration/dimensions;
- integrity information where appropriate;
- hard limits.

Do not base64 large media inside normal JSON events unless a benchmark shows that it is acceptable.

## 9. Transport decision is pending

Candidates must be tested on actual hardware.

Possibilities include:
- ordinary HTTP for request/response;
- long polling;
- a persistent socket protocol;
- WebSocket if the available 3DS stack is reliable enough.

Remote transport must be encrypted with a standard, reviewed security design.

Do not invent custom cryptography to work around platform limitations.
