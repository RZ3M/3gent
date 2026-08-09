# ADR-0003 — Pairing implementation

**Status:** Accepted for the current build; enforcement deferred

Implements ADR-0001 (QR pairing as default). ADR-0001 decided *that* pairing is
QR-first; this record decides *how*, and what was deliberately not done.

## Context

Until this change the handheld dialled a compile-time `SERVER_HOST`. Changing
machines meant rebuilding the application, and every request was anonymous. That
was acceptable while the goal was proving the agent loop on a trusted LAN; it is
not a product.

## Decision

### The endpoint becomes runtime state

`client-3ds/source/main.c` keeps `active_host`/`active_port`/`active_push_port`,
seeded from the saved pairing and falling back to the build constants when
nothing is paired. Everything below the start screen is unchanged and does not
know where the endpoint came from.

Pooled sockets are keyed to nothing but their own liveness, so
`network_reset_connections()` drops them whenever the active endpoint changes.
Without that, the first request after pairing would be written down a connection
opened to the previous machine.

### A start screen owns endpoint selection

`UI_SCREEN_HOME` is the entry point: connect, pair by QR, pair by typed code,
forget this machine, exit. `START` in the agent loop now returns here instead of
quitting, which is why the main screen's chip reads `Home`.

### Bootstrap carries no credential

The QR encodes an endpoint plus a one-time code (`docs/PROTOCOL.md` §3.5). The
handheld exchanges it for a revocable device token over `POST /v1/pair`; only the
result reaches the SD card. The bootstrap is never persisted.

### The bridge encodes its own QR

The bridge has no runtime dependencies and this keeps it that way: a byte-mode
level-M encoder limited to versions 1–10, which is all a fixed short pairing URL
needs. Level M over L buys error-correction headroom for a 2011 camera; the
version ceiling keeps the module grid coarse enough to resolve at 400×240.

The encoder is validated two ways. `bridge/test/pairing.test.ts` round-trips
matrices through an independent reader that shares no code with the encoder.
`tools/qr-check/` then decodes the bridge's output using the *same vendored
quirc build that runs on the device*, which is the compatibility question that
actually matters and the one a unit test cannot answer.

### Decoding runs off the interactive loop

A 400×240 decode costs far more than a frame's budget on Old 3DS. The scanner
hands frames to a lower-priority worker thread and never blocks; the viewfinder
and the network pump keep running at frame rate. One frame is in flight at a
time, so there is no queue to bound.

Camera delivery and re-arming are separate calls. The camera only writes to the
buffer while a transfer is armed, so a delivered frame is stable until it is
released — that is what makes single-buffered capture race-free.

## Not done

- **Enforcement by default.** `--require-pairing` exists and is off. See D-022:
  a bearer token over a plaintext link is an identity, not a boundary, and
  should become mandatory alongside secure transport (D-P11), not before.
- **An endpoint identity fingerprint.** The `f` field is reserved in the payload
  grammar and deliberately unused; emitting a fingerprint that nothing verifies
  would be decoration.
- **Relay pairing.** The QR carries a direct host and port. Pairing through the
  reverse relay (D-019) needs the relay to mint bootstraps, which is a separate
  decision.

## Consequences

- The application no longer needs rebuilding to point at another machine.
- A lost handheld is answered by `--revoke-device`, not by rotating everything.
- Two new failure surfaces need hardware evidence: camera scan reliability
  (R-006) and SD-card credential persistence across sleep and relaunch.
