# 3gent desktop bridge — functional core hardware-test build

This is the first production-shaped desktop boundary for 3gent. It is a
TypeScript/Node service with an agent-agnostic protocol, a deterministic fake
adapter, and a real Codex adapter over the supported local stdio app-server.

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

That command keeps the deterministic fake adapter. To control actual Codex
tasks, start the bridge from the repository you want a new task to use:

```sh
npm start -- --adapter codex --workspace /path/to/project --host 0.0.0.0 --port 8080 --push-port 8081
```

`--adapter codex` spawns `codex app-server --listen stdio://`, lists up to 64
recent tasks, and accepts both new and resumed tasks. Use
`--codex-executable /path/to/codex` only when `codex` is not on `PATH`.

Real voice input requires a transcriber. For OpenAI's audio transcription API,
keep the credential on the laptop and run:

```sh
OPENAI_API_KEY=... npm start -- --adapter codex --transcriber openai --workspace /path/to/project --host 0.0.0.0
```

The default model is `gpt-4o-mini-transcribe`; override it with
`--transcription-model`. A fully local/self-hosted recognizer can be connected
without a shell by passing its executable and repeatable arguments:

```sh
npm start -- --adapter codex --transcriber command \
  --transcription-command /path/to/transcribe \
  --transcription-arg --model --transcription-arg /path/to/model
```

The finalized WAV path is appended as the last argument. Stdout must contain
only the transcript. It is bounded to 1,600 UTF-8 bytes for handheld review.
The fake adapter uses a deterministic mock transcript unless overridden.

## Self-hosted reverse relay

The current remote test path is a bounded reverse TCP relay. It carries the
same HTTP media/session traffic and pushed control connection, while the laptop
opens all relay-facing uplinks outbound. On a self-hosted internet server:

```sh
export THREEGENT_RELAY_TOKEN='replace-with-at-least-16-random-bytes'
npm run relay -- --host 0.0.0.0 --unsafe-public
```

On the laptop bridge, use the same environment value:

```sh
export THREEGENT_RELAY_TOKEN='replace-with-at-least-16-random-bytes'
npm start -- --adapter codex --transcriber command --transcription-command /path/to/transcribe \
  --workspace /path/to/project --relay-host relay.example.net --host 127.0.0.1
```

Defaults are public 3DS ports `9080`/`9081` and bridge uplink ports
`9180`/`9181`. Build the 3DS client with the relay's numeric IPv4 address and
public ports. The bridge keeps four bounded HTTP/media uplinks and one pushed
control uplink available, replenishing each with capped reconnect backoff.

This is deliberately an insecure hardware-test relay: the bridge uplinks use a
token, but the 3DS-facing sockets are plaintext and unauthenticated. The relay
refuses to start without `--unsafe-public`. Use a private/test VPS or restrictive
firewall and disposable content. QR pairing, device credentials and TLS remain
required before any public release.

## Photo captures

The 3DS uploads exactly 400×240 RGB565 data to the media route. The bridge saves
an immutable bounded top-down bitfield BMP for each task's pending capture and
also publishes `bridge/data/latest.bmp` as a diagnostic copy. The next text or
reviewed-voice prompt consumes only that task's attachment, then removes its
immutable file. The Codex adapter maps the attachment to app-server `localImage`
input.

The 3DS build targets HTTP/audio port 8080 and pushed-control port 8081. Binding
to `0.0.0.0` exposes both unauthenticated Stage 1.5 services to the local
network; use a trusted LAN and disposable prompts. Never expose these ports to
the internet.

The fake bridge exposes one session, `ses_fake_local`. Normal text produces bounded
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

Forty-four tests cover HTTP protocol enforcement, session discovery/lifecycle, event
ordering/replay, command deduplication, fake text streaming, interruption,
approvals, audio/WAV capture, cursor failures, safe/verbose logging, immediate
pushed events, command replay across reconnect, resync snapshots, oversized
frames, heartbeat cleanup, event-history byte bounds, Codex JSON-RPC framing,
opaque thread mapping, streamed deltas, diff summaries, and one-shot approvals.
They also cover delayed-adapter heartbeat liveness, lifecycle races, approval
bounds, local command transcription, and an OpenAI-compatible multipart request.

This service is not safe for remote exposure. Pairing, credentials, policy, and
encrypted remote transport are later stages.
