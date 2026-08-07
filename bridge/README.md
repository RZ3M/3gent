# 3gent desktop bridge — Stage 1

This is the first production-shaped desktop boundary for 3gent. It is a
TypeScript/Node service with an agent-agnostic protocol and a deterministic fake
adapter. It deliberately does not invoke Codex or another real coding agent yet.

## Requirements

- Node.js 22 or newer
- npm

## Install and run

```sh
cd bridge
npm ci
npm run build
npm start -- --host 0.0.0.0 --port 8080
```

The 3DS build currently targets port 8080. Binding to `0.0.0.0` exposes this
unauthenticated Stage 1 service to the local network; use disposable prompts.

The bridge exposes one session, `ses_fake_local`. Normal text produces bounded
structured text deltas. Text containing `approve` or `approval` pauses at a fake
approval. Audio is saved to `bridge/data/latest.wav`, emits capture metadata,
and uses a mock transcript to exercise the same turn pipeline. The terminal logs
accepted/replayed command kinds and byte counts without printing prompt content.

## Test

```sh
npm test
```

Tests cover protocol version enforcement, session discovery, event ordering and
replay, command deduplication, fake text streaming, interruption, approvals,
audio/WAV capture, and expired/ahead event cursors.

This service is not safe for remote exposure. Pairing, credentials, policy, and
encrypted remote transport are later stages.
