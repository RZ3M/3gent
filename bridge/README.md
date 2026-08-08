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

## Verbose protocol logging

Add `--verbose` when you want to inspect the meaningful development exchange:

```sh
npm start -- --host 0.0.0.0 --port 8080 --verbose
```

Verbose mode logs meaningful requests and responses, exact text captures,
approval JSON, command acknowledgements, and complete event envelopes.
Microphone PCM is shown as received chunk sizes and running byte totals rather
than raw binary samples. Empty event polls are hidden, so an idle 3DS no longer
scrolls the terminal continuously.

To diagnose polling itself, use the deliberately noisy option:

```sh
npm start -- --host 0.0.0.0 --port 8080 --verbose-polls
```

`--verbose-polls` implies `--verbose` and additionally logs every empty request
and zero-event response. Client `0.1.1-stage1` checks roughly 10 times/second
while an agent is working, 4/second while an approval is pending, and once per
second while idle. Those lines are bridge reads for outbound agent events, not
unsolicited 3DS button input.

Verbose logs can contain sensitive prompts, agent output, commands, paths, and
future adapter content. Enable the flag only for deliberate local debugging and
do not publish or commit captured terminal logs.

## Test

```sh
npm test
```

Tests cover protocol version enforcement, session discovery, event ordering and
replay, command deduplication, fake text streaming, interruption, approvals,
audio/WAV capture, expired/ahead event cursors, safe default logging, and full
verbose text/event/audio diagnostics, suppression of empty polls, and the
explicit noisy-poll override.

This service is not safe for remote exposure. Pairing, credentials, policy, and
encrypted remote transport are later stages.
