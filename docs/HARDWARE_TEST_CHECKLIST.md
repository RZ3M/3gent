# 3gent 0.6 functional-core hardware checklist

This is the source of truth for the physical Nintendo 3DS validation pass. Host
tests cannot prove camera, MICU, Wi-Fi, lid/sleep, framebuffer, keyboard or
Homebrew Launcher behavior, so do not infer a pass from CI.

Record the 3DS model, system/homebrew versions, host OS, Node, Codex,
devkitARM/libctru versions, network type and measured times. Use a disposable
repository and disposable prompts/media throughout this build.

## A. Host and build gate

1. From `bridge/`, run `npm ci` and `npm test`. Require all 44 tests to pass.
2. Run `codex --version`; record the installed version and confirm ordinary
   Codex tasks work on the laptop before involving 3gent.
3. From `client-3ds/`, run:

   ```sh
   make clean
   make SERVER_HOST=10.0.0.196 SERVER_PORT=8080 PUSH_PORT=8081
   ```

   Replace the address with the laptop's numeric LAN IPv4. Require a clean build.
4. Copy `3gent.3dsx` and `3gent.smdh` to `/3ds/3gent/` on the SD card. Confirm
   Homebrew Launcher shows version `0.6.1-hwtest`.
5. Create a disposable Git repository/worktree containing at least one text file.

## B. Deterministic fake-adapter regression

Start:

```sh
cd bridge
npm start -- --adapter fake --host 0.0.0.0 --port 8080 --push-port 8081 --verbose
```

1. Launch 3gent. Confirm `Local fake agent` appears; select it with `A`.
2. Confirm the push link becomes `ready`, state becomes `idle`, and the ordinary
   verbose log does not continuously print ping/pong or event polls.
3. Send typed text. Require near-immediate command acceptance, incremental fake
   response text, completion and return to idle.
4. Hold Up/Down on a long response. Require smooth repeat and no frame freeze.
5. Press `X` while idle, approve the fake request with `X`, and require exactly
   one continuation. Repeat and decline with `B`.
6. Start a response and interrupt with `B`. Require an interrupted marker and idle.
7. Hold `R`, speak for at least five seconds, and release. The mock transcript
   must appear without starting a turn. Cancel with `B`; repeat, edit with `Y`,
   then send with `A`. Require exactly one turn using the reviewed text.
8. Leave the keyboard open for 20 seconds, cancel, and require push reconnection
   without lost/duplicated later output.

## C. Real Codex task lifecycle

Start from `bridge/`:

```sh
npm start -- --adapter codex \
  --workspace /absolute/path/to/disposable/repository \
  --host 0.0.0.0 --port 8080 --push-port 8081 --verbose
```

1. Launch 3gent. Require up to six recent Codex tasks with readable labels.
2. Move Up/Down across the list. Resume one with `A`; require pushed link ready.
3. Relaunch and press `X` in the chooser. Confirm a new Codex task is created in
   exactly the bridge `--workspace`, not an arbitrary 3DS-provided directory.
4. Send a harmless typed prompt. Require:
   - one accepted capture;
   - state `working` while active;
   - agent text arriving incrementally;
   - complete readable output with held scrolling;
   - state returning to `idle`;
   - no Codex UUID shown on the 3DS.
5. Resume that same task after restarting both app and bridge. Send a contextual
   follow-up and verify Codex retained the task history.
6. Start a slow task and press `B`. Require the matching turn to stop and no late
   response to reactivate it.
7. Ask Codex to make a small disposable edit. Require a visible bounded diff
   summary with correct file/addition/deletion counts; inspect the actual diff on
   the laptop.
8. Use a desktop Codex approval policy and a controlled command known to prompt
   in the disposable repository. Verify the exact bounded summary on 3DS,
   approve once with `X`, and ensure it executes once. Trigger another and decline
   with `B`. Do not use a vague “ask for approval” prompt as proof.
9. Confirm existing Codex approval/reviewer policy was not silently changed by
   resuming or sending through 3gent.
10. Trigger an error (for example, safely rename the disposable workspace after
    selecting it). Require a bounded visible error and responsive controls.

## D. Real voice transcription and review

Choose one real backend.

OpenAI-hosted example (credential remains only on the laptop):

```sh
export OPENAI_API_KEY='...'
npm start -- --adapter codex --transcriber openai \
  --workspace /absolute/path/to/disposable/repository \
  --host 0.0.0.0 --port 8080 --push-port 8081 --verbose
```

For a local/self-hosted recognizer, use `--transcriber command`,
`--transcription-command`, and repeat `--transcription-arg`; the bridge appends
the WAV path as the final argument and reads transcript-only stdout.

1. Record one to two seconds. Require capture to start promptly, PCM duration and
   level to move, then a non-empty transcript or clear empty-audio error.
2. Record a normal 5–15 second request. Require control push to remain ready while
   audio streams and while transcription waits.
3. On release, require the UI to remain interactive for up to the separate
   120-second transcription deadline. Verify `bridge/data/latest.wav` duration
   and intelligibility.
4. Require transcript text on the 3DS and verify no Codex turn starts yet.
5. Press `B`; require cancellation and no turn.
6. Record again, press `Y`, edit through the native keyboard, and cancel the edit
   once. Reopen edit, accept, verify reviewed text, then press `A`. Require exactly
   one Codex turn containing the final edited transcript.
7. Test punctuation, numbers and non-ASCII speech. Record any replacement glyphs
   separately from transcription errors.
8. Stop the transcription backend during finalization. Require a bounded error,
   no fake response and a successful later retry after restoration.
9. Record enough speech to exceed the 1,600-byte review bound. Require explicit
   `TRANSCRIPT_TOO_LARGE`; no partial text may be sent to Codex.
10. Leave the app idle for eleven minutes, then record again without restarting.

## E. Camera/photo capture

1. With the agent idle and no transcript pending, press `L`. Record camera init
   time and shutter latency.
2. Confirm the top-screen preview has correct orientation and believable red,
   green and blue colors. Photograph a known color/orientation target.
3. Press `B`; require cancellation with no upload or attachment.
4. Capture again and press `A`. Require smooth progress from 0 to exactly 192,000
   bytes while push remains alive.
5. Open `bridge/data/latest.bmp` on the laptop. Require 400×240 dimensions,
   correct orientation/colors and no truncation.
6. Confirm the 3DS says the photo is attached to the next prompt. Send a prompt
   asking Codex to describe a distinctive visible feature.
7. Require `capture.attached`, one image in the Codex turn, and a relevant visual
   response. The subsequent text prompt must not receive the old photo.
8. Capture a new photo while an older one is pending in the same task. Confirm
   that task's bounded pending slot is intentionally replaced by the newest image.
9. Upload a distinctive photo in task A, restart the app and upload a different
   photo in task B, then send each task's next prompt. Require each task to receive
   its own photo; `latest.bmp` may show the newest diagnostic copy only.
10. Exercise camera after microphone use and microphone after camera use. Require
   both services to remain available.
11. Cancel during upload with `START`; require no replacement of the last complete
    BMP and no wedged media connection on the next launch.

## F. Local failure and reconnect matrix

1. Stop the bridge while idle. Require heartbeat detection, reconnect backoff,
   working scrolling/buttons and no HTTP event polls.
2. Restart it. Require cursor replay or a visible resync marker, never silent gaps.
3. Stop the bridge during agent output; restart before history expires. Require
   ordered, non-duplicated deltas.
4. Run with `--push-test-drop-next-ack`, send one prompt, and require one turn plus
   a replayed/duplicate acknowledgement after reconnect.
5. Run with `--push-test-blackhole --verbose-polls`. Require the 3DS to leave ready
   after roughly eight seconds and reconnect without freezing. Remove the flag.
6. Stop the bridge, submit one text command, attempt a second, then restore it.
   Require one bounded queued command, clear busy feedback and exactly-once send.
7. Close the lid for 2–5 seconds during output, then for more than 12 seconds.
   Require bridge peer cleanup and automatic replay/resync after reopening.
8. Turn Wi-Fi off/on or move briefly out of range. Record reconnect time and
   confirm no input/render freeze.
9. Run text, audio finalization and pushed output concurrently where allowed.
   Require heartbeats and assistant events not to starve.
10. Press `START` from ordinary UI and require clean Homebrew Launcher return.

## G. Self-hosted remote relay (explicitly unsafe test build)

Use a disposable/private test server. The public 3DS sockets are plaintext and
unauthenticated. Do not transmit credentials, proprietary code or sensitive audio.

On the self-hosted server:

```sh
cd bridge
npm ci
npm run build
export THREEGENT_RELAY_TOKEN="$(openssl rand -hex 32)"
npm run relay -- --host 0.0.0.0 --unsafe-public
```

Allow TCP `9080`, `9081`, `9180`, and `9181` only for the test. On the laptop,
use the exact same token and start the bridge with `--relay-host`:

```sh
export THREEGENT_RELAY_TOKEN='same value'
npm start -- --adapter codex --transcriber openai \
  --workspace /absolute/path/to/disposable/repository \
  --host 127.0.0.1 --port 8080 --push-port 8081 \
  --relay-host relay.example.net
```

Rebuild the 3DS client with the relay's numeric IPv4 and public ports 9080/9081.

1. Put the 3DS on a different network or compatible 2.4 GHz hotspot from the
   laptop. Confirm task discovery and resume through the relay.
2. Repeat typed response streaming, interrupt and both approval choices.
3. Repeat real voice record → transcribe → review → edit/cancel/send.
4. Repeat photo preview → upload → next-prompt attachment.
5. Disconnect/reconnect the 3DS network. Require push replay/resync and fresh
   HTTP/media tunnels without restarting the relay.
6. Stop/restart the laptop bridge. Require outbound tunnel pools to replenish
   automatically and the 3DS to recover.
7. Stop/restart the relay. Record bridge capped-backoff recovery time.
8. Attempt nine simultaneous waiting public connections on one channel. Confirm
   the relay rejects excess waiters and remains responsive.
9. Use a wrong bridge token. Confirm no tunnel is paired and no token is logged.
10. After testing, close the four firewall ports and remove the relay process.

## H. Pass report

For each failed item capture:

- checklist item;
- Old/New 3DS model;
- exact screen state and bridge/relay summary log;
- elapsed time;
- whether retry recovered;
- whether a full reboot was needed;
- resulting WAV/BMP when relevant.

Do not call the build hardware-ready until every applicable A–G item passes or
is explicitly waived with a documented reason. This checklist does not approve
the relay for public/security-sensitive use; QR pairing, device credentials,
revocation and production TLS remain a separate release gate.
