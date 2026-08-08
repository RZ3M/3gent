# 3gent desktop bridge — Stage 1.5

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
npm start -- --host 0.0.0.0 --port 8080 --push-port 8081
```

The 3DS build targets HTTP/audio port 8080 and pushed-control port 8081. Binding
to `0.0.0.0` exposes both unauthenticated Stage 1.5 services to the local
network; use a trusted LAN and disposable prompts. Never expose these ports to
the internet.

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
than raw binary samples. Push ping/pong frames and legacy empty event polls are
hidden, so an idle 3DS does not scroll the terminal continuously.

To diagnose polling itself, use the deliberately noisy option:

```sh
npm start -- --host 0.0.0.0 --port 8080 --verbose-polls
```

`--verbose-polls` implies `--verbose` and additionally logs every legacy empty
request/response and each push heartbeat. Client `0.1.2-stage1.5` does not poll;
this option mainly exists for fixture and liveness diagnostics.

## Hardware fault-injection flags

These explicit development flags make reconnect boundaries reproducible. Never
use them for ordinary operation:

- `--fake-delta-ms 1000`: slow fake response pieces so a control-link drop can
  occur during a turn;
- `--push-test-blackhole`: accept the pushed link but deliberately suppress
  post-ready outbound frames, proving the 3DS eight-second heartbeat timeout;
- `--push-test-drop-next-ack`: execute the next new control command, then close
  the link before its acknowledgement, proving same-ID retry/deduplication.

The bridge prints a prominent warning when a push fault is enabled. Each flag is
also covered by deterministic host-side behavior where practical.

Verbose logs can contain sensitive prompts, agent output, commands, paths, and
future adapter content. Enable the flag only for deliberate local debugging and
do not publish or commit captured terminal logs.

## Test

```sh
npm test
```

Twenty-three tests cover HTTP protocol enforcement, session discovery, event
ordering/replay, command deduplication, fake text streaming, interruption,
approvals, audio/WAV capture, cursor failures, safe/verbose logging, immediate
pushed events, command replay across reconnect, resync snapshots, oversized
frames, heartbeat cleanup, and event-history byte bounds.

This service is not safe for remote exposure. Pairing, credentials, policy, and
encrypted remote transport are later stages.
