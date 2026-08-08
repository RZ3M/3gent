# 3gent functional core hardware-test client

This client connects the handheld feasibility work to the TypeScript desktop
bridge and either its deterministic fake agent or local Codex adapter. It is still a local-development build,
not a remotely secure product.

The client currently provides:

- native software-keyboard text capture;
- bounded-memory live microphone streaming using signed PCM16 mono at 16,364 Hz;
- one pre-warmed HTTP/1.1 audio connection plus the pushed control link;
- non-blocking per-frame runtime connection, send, receive, and finalization;
- protocol-v1 commands with unique command IDs;
- a persistent bidirectional pushed-control link with no event polling;
- heartbeat, jittered reconnect, applied-cursor replay, and visible resync;
- safe retry of one unacknowledged command with its original command ID;
- recent-task selection plus new-task creation in the configured bridge workspace;
- incremental agent text, status, diff summary, approval, and interruption UI;
- 400×240 outer-camera capture, preview, bounded upload and next-prompt attachment;
- fixed 4 KiB response storage and held-button scroll navigation.

Stage 0 keyboard, LAN, streamed rendering, scrolling, sustained microphone
capture, and warm-connection latency have passed on physical hardware. The first
Stage 1 hardware run was reported as working well. The detailed
reliability and usability checklist below remains the source of truth for the
specific cases that have not yet been reported separately.

## Prerequisites

Install the current official devkitPro toolchain. On macOS, follow the
[devkitPro getting-started instructions](https://devkitpro.org/wiki/Getting_Started),
install the `3ds-dev` package group, and restart the shell so `DEVKITPRO` and
`DEVKITARM` are available.

The desktop bridge requires Node.js 22 or newer and npm.

## Build the bridge and client

First find the LAN IPv4 address of the computer that will run the bridge. Build
the 3DS client with that numeric address:

```sh
cd client-3ds
make SERVER_HOST=192.168.1.42 SERVER_PORT=8080 PUSH_PORT=8081
```

Replace `192.168.1.42` with the computer's address. Runtime configuration and
pairing come in later stages. The build produces `3gent.3dsx`, `3gent.smdh`,
`3gent.elf`, and `build/3gent.map`.

From the repository root, install, test, and start the functional Codex bridge:

```sh
cd bridge
npm ci
npm test
OPENAI_API_KEY=... npm start -- --adapter codex --transcriber openai \
  --workspace /path/to/project --host 0.0.0.0 --port 8080 --push-port 8081
```

That command enables the complete typed, voice, and photo path. To test only
typed input and photos, omit `OPENAI_API_KEY=... --transcriber openai`; microphone
finalization will then clearly report that transcription is not configured. A
local/self-hosted transcription command is documented in `bridge/README.md`.

For full request, prompt, acknowledgement, event, and audio-chunk diagnostics,
append `--verbose`. Empty event checks are hidden so the terminal shows only
meaningful traffic. Push ping/pong heartbeats are also hidden. Use
`--verbose-polls` only for deliberately noisy transport diagnostics; it implies
verbose mode and restores legacy empty HTTP polls and push heartbeats. Both
modes can print sensitive prompt and agent content and should be used only for
deliberate local debugging.

Binding to `0.0.0.0` exposes an unauthenticated development service to the local
network. Use a trusted LAN and disposable prompts. Do not expose it to the
internet.

## Install and launch

Copy `3gent.3dsx` and `3gent.smdh` into `3ds/3gent/` on the SD card, then launch
3gent through the Homebrew Launcher.

At launch, Up/Down chooses a recent task, `A` resumes it, and `X` creates a new
task in `--workspace`. Runtime controls are:

- `A`: open the native keyboard and send text to the selected agent;
- `X`: send an approval-test prompt, or approve once when one is pending;
- `B`: interrupt an active turn, or decline a pending approval;
- hold `R`: stream microphone audio, then review its transcript on release;
- `L`: capture and preview a photo, then attach it to the next prompt;
- D-pad/Circle Pad Up/Down: scroll the response, including held repeat;
- `START`: exit.

Successful audio is saved as `bridge/data/latest.wav`, transcribed by the
configured laptop backend, and returned for review. `A` sends the reviewed
transcript, `Y` edits it in the native keyboard, and `B` cancels it. Recording
never starts an agent turn automatically.

## Focused Stage 2 Codex checklist

Run this before the longer Stage 1.5 reliability list below:

1. Start the bridge with `--adapter codex --workspace` pointing at a disposable
   test repository; confirm the bridge reports `Adapter: Codex app-server`.
2. Launch `0.6.0-hwtest`; confirm no fake session ID is required and up to six
   recent Codex tasks appear.
3. Move the selection with tapped Up/Down, resume a task with `A`, and confirm
   the pushed link becomes ready.
4. Relaunch, press `X` in the task chooser, and confirm a new Codex task appears
   on the laptop under the configured workspace.
5. Send a harmless typed request. Confirm text deltas appear while Codex works,
   completion returns the handheld to idle, and no Codex UUID appears on-screen.
6. Ask Codex to edit a disposable file. Confirm response text and the compact
   changed-file/addition/deletion summary arrive.
7. In a disposable repository whose desktop Codex policy asks before the chosen
   command, request that exact command. Confirm the bounded summary appears;
   approve once with `X` and verify Codex proceeds once.
8. Trigger another approval, decline with `B`, and verify Codex does not execute it.
9. Start a longer request and press `B`; confirm the matching Codex turn is
   interrupted and the session returns to idle.
10. Stop/restart the bridge during output and confirm replay/resync keeps the UI
    responsive. Then resume the same task and send a follow-up successfully.
11. Hold `R`, speak a short request, and release. Confirm the recording is
    transcribed but no Codex turn starts. Review the text, press `Y` to edit and
    cancel once, then record again and press `A` to send. Only that final
    transcript should create a Codex turn.
12. Press `L`, confirm the shutter/preview, cancel once with `B`, then capture
    again and accept with `A`. Confirm upload progress reaches 192,000 bytes.
13. Type a prompt asking Codex to inspect the photo. Confirm the handheld marks
    the attachment consumed, `bridge/data/latest.bmp` opens correctly, and Codex
    receives one image with that prompt. The following prompt must not reuse it.

## Self-hosted relay hardware test

Run the relay and bridge exactly as documented in `bridge/README.md`, then build
with the relay server's numeric IPv4 address and public ports:

```sh
make SERVER_HOST=203.0.113.10 SERVER_PORT=9080 PUSH_PORT=9081
```

The current relay is an explicit plaintext test build and must not carry secrets
or be treated as production security. Test from a different network/hotspot:

1. Confirm the task chooser loads through the HTTP reverse tunnel.
2. Resume a task and confirm pushed text/state works in both directions.
3. Record, transcribe, review and send voice through the relay.
4. Capture/upload a photo and consume it with the next prompt.
5. Stop the laptop bridge; verify the 3DS remains responsive and reconnects
   after the bridge returns without restarting the relay or handheld app.

## Physical Stage 1.5 verification checklist

Record the hardware model, host OS, tool versions, and displayed timings.

1. Start the TypeScript bridge and confirm it prints its listening address.
2. Launch `0.1.2-stage1.5` through the Homebrew Launcher.
3. Confirm startup reports the audio link warm, `Control push: ready`, and agent
   state `idle`.
4. Press `A`, cancel the keyboard, and confirm the app returns safely.
5. Press `A`, send `hello from 3DS`, and confirm the bridge accepts it almost
   immediately.
6. Confirm the top screen moves through working/receiving/idle states and shows
   the fake response incrementally rather than all at once.
7. Hold Up/Down and confirm scroll repeat still works while the response remains
   readable.
8. Send a longer prompt, press `B` while text is arriving, and confirm the turn
   stops with an interruption message and returns to idle.
9. While idle, press `X`. Confirm an approval appears with the summary
   `Run the fake Stage 1 test command`.
10. Press `X` again to approve once. Confirm the fake turn resumes and completes.
11. Start the approval demo again, then press `B`. Confirm decline is shown and
    the fake command is not run.
12. While idle, hold `R` and speak for at least fifteen seconds. Confirm the
    PCM duration continues increasing and audio starts without the old delay.
13. Release `R`; confirm the fake voice response streams on screen and
    `bridge/data/latest.wav` has the expected duration and intelligible audio.
14. Leave the app idle for at least eleven minutes. Confirm the bridge terminal
    stays quiet in ordinary verbose mode, then send another prompt and start a
    short audio capture without restarting the app.
15. With a scrollable response on screen, stop the bridge. Hold Up/Down and
    confirm the screen and held-repeat navigation never freeze while the pushed
    link detects heartbeat loss and enters reconnect backoff.
16. Restart the bridge. Confirm `Control push` returns to ready, the app visibly
    reports a session resync if the bridge cursor restarted, and a new prompt
    displays the complete fake-agent response without restarting the app.
17. Stop the bridge during a short microphone capture, release `R`, and confirm
    the finalizing UI remains live until it reports a bounded stream error.
18. Close and reopen the 3DS lid during an active turn and record the resulting
    stop, replay, resync, or resume behavior.
19. Press `START` and confirm a clean return to the Homebrew Launcher.
20. Restart the bridge with `--verbose-polls`. Leave the 3DS idle for at least
    five seconds; confirm `ping`/`pong` appears, `Control push` remains ready,
    and no `GET /v1/events` request appears at any time.
21. Press `A`, leave the native keyboard open for at least twenty seconds, then
    cancel. The app deliberately reconnects around this modal system screen;
    confirm it returns to `Control push: ready` without freezing or losing later
    agent events.
22. Restart with `--fake-delta-ms 1000`, start a turn, close the 3DS lid for two
    to five seconds during output, then reopen it before the bridge exits. Confirm
    the same bridge replays ordered deltas with no duplicate or missing text.
23. Restart with `--push-test-blackhole --verbose-polls`. Confirm the client
    detects about eight seconds without inbound traffic, leaves `ready`, and
    reconnects without blocking scrolling or buttons. Remove the flag afterward.
24. Restart with `--push-test-drop-next-ack --verbose`, then submit one prompt.
    Confirm the bridge executes it once, drops the link before acknowledgement,
    and reports the retried command as replayed/duplicate after reconnect. The
    3DS must show only one turn.
25. Stop the bridge, type and submit one prompt while `Control push` is retrying,
    then attempt a second prompt. Confirm one bounded command is queued, the
    second attempt gives clear busy feedback, scrolling remains responsive, and
    restoring the bridge sends exactly the first prompt once.
26. Close the lid for more than twelve seconds. With verbose logging, confirm the
    bridge releases the silent peer; reopen the lid and confirm automatic cursor
    resume or visible resync.
27. With `--verbose-polls`, hold `R` and speak for more than five seconds. Confirm
    push heartbeats and any agent events continue while PCM streams/finalizes and
    neither direction blocks the other.

Cursor-expired recovery, coalesced TCP frames, lost acknowledgements, slow-peer
backpressure, byte-budget eviction, and malformed/oversized frames also have
deterministic host tests. The physical checklist concentrates on behavior that
depends on real 3DS Wi-Fi, sleep, modal keyboard, input, and rendering.

Do not mark the Stage 1.5 pushed link complete until every applicable step
passes. Host-only fault-injection checks are recorded separately in the bridge
test suite; hardware results must never be inferred from those tests.

The current development client does not poll for events. The bridge pushes them
over port 8081. After three seconds without inbound traffic the 3DS sends a
heartbeat; after eight seconds without any response it reconnects with jittered
250 ms-to-ten-second backoff. Ordinary verbose logs suppress heartbeats.

For the consolidated real-Codex, voice, photo, failure and remote-relay pass,
use `docs/HARDWARE_TEST_CHECKLIST.md`; it supersedes overlapping historical
steps in this Stage 1.5 regression list.

## Troubleshooting

- `DEVKITARM is not set`: install the official `3ds-dev` package group and
  restart the shell or Mac.
- `connect timed out`: verify the compiled IP, bridge bind address, firewall,
  and that both devices can communicate on the same LAN.
- `server returned HTTP 404` or `426`: stop the Python Stage 0 server and launch
  the current TypeScript bridge from this checkout.
- `server closed warm connection`: restart the current bridge and retry.
- `response exceeded the bounded buffer`: report the action and server log; the
  client intentionally rejects oversized event batches.
- `Agent: offline`: keep the bridge running, retry an action, and record whether
  it reconnects.
- `Mic: unavailable`: exit other software using the microphone, record the
  hexadecimal service error, and restart the app.
- `Audio stream error`: restore the bridge and record again. Partial captures
  are discarded and do not replace the last completed WAV.
