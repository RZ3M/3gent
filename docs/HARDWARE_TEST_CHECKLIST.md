# 3gent 0.8 functional-core hardware checklist

This is the source of truth for the physical Nintendo 3DS validation pass. Host
tests cannot prove camera, MICU, Wi-Fi, lid/sleep, framebuffer, keyboard or
Homebrew Launcher behavior, so do not infer a pass from CI.

Record the 3DS model, system/homebrew versions, host OS, Node, Codex,
devkitARM/libctru versions, network type and measured times. Use a disposable
repository and disposable prompts/media throughout this build.

## A. Host and build gate

1. From `bridge/`, run `npm ci` and `npm test`. Require all 55 tests to pass.
2. Run `make -C tools/qr-check run`. This decodes the bridge's pairing QR with
   the same vendored quirc build the handheld uses; require it to print the
   payload back. A failure here means no amount of camera work will pair.
3. Run `codex --version`; record the installed version and confirm ordinary
   Codex tasks work on the laptop before involving 3gent.
4. From `client-3ds/`, run:

   ```sh
   make clean
   make SERVER_HOST=10.0.0.196 SERVER_PORT=8080 PUSH_PORT=8081
   ```

   The address is now only the fallback for the start screen's "Connect" row
   when nothing is paired; pairing overrides it. Require a clean build.
5. Copy `3gent.3dsx` and `3gent.smdh` to `/3ds/3gent/` on the SD card. Confirm
   Homebrew Launcher shows version `0.9.0-ui`.
6. Create a disposable Git repository/worktree containing at least one text file.

## B. Interface rendering gate

New in `0.7.0-gui`. The interface is now drawn by citro2d on the GPU, so these
must pass before the behavioural sections mean anything. Review
`tools/ui-preview/out/index.html` first so you know what each state should look
like; anything below that disagrees with the preview is a hardware finding.

1. Launch the app. Confirm the boot screen renders the wordmark and gradient
   rule cleanly, with no tearing, no stuck frame and no console text anywhere.
2. In the task chooser, confirm the highlighted row is legible, the selection bar
   tracks Up/Down immediately, and long task labels truncate with an ellipsis
   instead of overflowing the panel.
3. Resume a task. Read the top screen from a normal handheld distance and
   confirm the response body is comfortably legible on a physical panel. Record
   the model, because Old and New 3DS panels differ. Text that is too small here
   is a scale change in `UI_SCALE_BODY`, not a waived item.
4. Leave the app idle for two minutes on the main view. Confirm there is no
   flicker, no drifting artefacts, and no visible frame-rate collapse. This is
   the R-020 regression check under the new renderer.
5. Send a prompt that produces more text than one screen. Confirm wrapping breaks
   on word boundaries, the scrollbar thumb appears and is proportional, held
   Up/Down repeats smoothly, and the footer reports the scrollback position.
6. Trigger an approval. Confirm the coral card takes the read surface, the
   command wraps and is fully readable, and the bottom screen shows two distinct
   choice buttons rather than one confirm.
7. Hold `R`. Confirm both screens switch to the recording treatment, the elapsed
   time advances smoothly, the level trace visibly responds to speech and
   silence, and the five-minute progress bar moves. Release and confirm both
   screens leave the recording state.
8. Press `L` and confirm the camera review shows the photo **the right way up**
   and the right way round. R-021 detects the texture orientation at run time; if
   this is wrong, capture a photo of the screen and record it against R-021.
9. During the upload, confirm the progress bar and percentage advance to 100.
10. Press `A` to open the keyboard, leave it for twenty seconds, then cancel.
    Confirm the interface returns fully drawn with the GPU intact — not black,
    not stale, not corrupted.
11. Close and reopen the lid on the main view, then again during a streaming
    turn. Confirm rendering resumes correctly in both cases.
12. Record whether battery drain during a ten-minute idle session is noticeably
    worse than the `0.6.2-hwtest` console build. Both screens now redraw every
    frame; if this is a problem it is a decision for D-020, not a bug fix.

## B2. QR pairing gate

New in `0.8.0-pairing`. This is the section with the least prior hardware
evidence (R-006), so record measurements, not just pass/fail.

Start the bridge with a pairing offer and a device store you can inspect:

```sh
npm start -- --host 0.0.0.0 --pair --devices-path ./data/devices.json
```

1. Confirm the bridge prints a QR code, a manual line, the payload and the SVG
   path, and that the advertised host is the laptop's LAN address rather than
   `0.0.0.0` or `127.0.0.1`. A wrong address here is a `--advertise-host` case.
2. Launch the app. Confirm it opens on the start screen — not on a connection
   attempt — and reports no machine paired.
3. Choose "Pair with a QR code". Confirm the viewfinder appears within about a
   second and the "looks" counter climbs, which is what proves the decoder
   thread is running rather than stalled.
4. Leave the viewfinder open for two minutes without scanning anything. Confirm
   the preview never freezes permanently. If the status line reports "camera
   restarted N times", record N — that is the R-006 buffer-error recovery doing
   its job, and a high rate is a finding even though the stream survives.
5. Open the generated `data/pairing.svg` full screen in a browser and scan it.
   Record the distance and time to decode. Repeat against the terminal QR and
   record whether it decodes at all — the answer decides whether the SVG is a
   convenience or a requirement.
6. Record scan behaviour at reduced screen brightness and at an angle, and on
   both Old and New 3DS if available. These are the R-006 numbers.
7. Confirm the outcome card names the bridge, and that the bridge logs
   `paired device dev_...`. Confirm `data/devices.json` contains a hash and
   **no** readable token.
8. Return to the start screen. Confirm it now names the paired machine, its
   address and the key ID.
9. Choose "Connect" and complete one ordinary text turn, to prove the paired
   endpoint drives real traffic and not just the display.
10. Press `START` from the agent loop. Confirm it returns to the start screen
    rather than quitting, and that "Connect" works a second time — this is the
    pooled-socket reset path.
11. Quit and relaunch. Confirm the pairing survives, with no rescan.
12. Point the camera at some unrelated QR code. Confirm the app says the code is
    not from 3gent and keeps scanning instead of failing out.
13. Let an offer expire (default 180 s), then scan it. Confirm the failure is
    named and recoverable in place with `A` or `Y`.
14. Choose "Enter a pairing code" and pair using the printed manual line on a
    fresh offer. Try it once with the dashes and once without.
15. With the bridge running under `--require-pairing`, run
    `npm start -- --list-devices` and then `--revoke-device <id>` from a second
    terminal. Confirm the handheld is refused on its next action **without
    restarting the bridge**, and that pairing again restores it.
16. Choose "Forget this machine". Confirm the start screen returns to the
    unpaired state and that the bridge still lists the device — forgetting is
    local, revocation is not.

## B3. Touch, navigation and task switching gate

New in `0.9.0-ui` (D-023, R-022). Everything here is a hardware-only question:
the preview proves the layout, not whether a thumb or stylus can hit it. Run it
with at least three tasks on the bridge, one of which is left blocked on an
approval.

1. On the task screen, press `B`. Confirm it returns to the task manager, not to
   the start screen, and that pressing `B` again returns to the start screen.
   `START` from the task screen must jump straight to the start screen.
2. With an approval pending, press `B` and confirm it declines rather than
   navigating away. Cancel a transcript with `B` and confirm the same.
3. Trigger an approval and press `A` as fast as you can react. Confirm the first
   press inside the arming window is refused with "Read it first, then approve"
   and a press a moment later approves. Record whether the delay felt protective
   or obstructive — this is the open question in D-023.
4. Tap each action-bar button with the stylus. Confirm the pressed state appears
   under the tip and the action fires on release. Then press a button, slide off
   it, and lift: the action must **not** fire.
5. Repeat step 4 with a thumb rather than the stylus, on the four-across idle
   bar. Record whether 70 px targets are usable that way; if they are not, the
   fix is fewer actions per row, not smaller text.
6. Tap each task tab in the rail and confirm it switches. Time one switch from
   tap to the first line of that task's output appearing. Record the number:
   R-022 asks whether stop/resume/replay feels like switching or reconnecting.
7. Press Left and Right on the D-pad, then on the Circle Pad. Confirm both walk
   the rail, wrap at the ends, and that the `2/6` position in the header agrees
   with the highlighted tab every time.
8. Leave a task blocked on an approval and switch away from it. Within about
   five seconds its tab must turn coral and pulse, the manager button must show
   a badge, and the top footer must read "1 other task needs you".
9. Let a task produce output while you are reading a different one. Confirm its
   tab gains the unread dot, and that the dot clears once you switch to it.
10. Switch to a task with a long history. Confirm the read surface shows its
    **most recent** output, not its oldest, and that scrolling back works.
11. Hold the push-to-talk panel with the stylus instead of `R`. Confirm
    recording starts on contact and stops on lift, and that the audio is
    complete. Then confirm touching anywhere during recording does not switch
    tasks.
12. With a response longer than one screen, tap each of the three scroll cluster
    buttons. Confirm page back, page forward and jump-to-newest, and that the
    cluster disappears entirely when the response fits.
13. Tap rows on the start screen and in the task manager. Confirm a tap both
    highlights and activates, and that the dimmed "Forget this machine" row does
    nothing when tapped.
14. Leave the app on the task screen for five minutes without touching it.
    Confirm the five-second background task refresh causes no visible frame-rate
    change, no log spam, and no interference with a streaming turn.

## C. Deterministic fake-adapter regression

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

## D. Real Codex task lifecycle

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

## E. Real voice transcription and review

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

## F. Camera/photo capture

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

## G. Local failure and reconnect matrix

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

## H. Self-hosted remote relay (explicitly unsafe test build)

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

## I. Pass report

For each failed item capture:

- checklist item;
- Old/New 3DS model;
- exact screen state and bridge/relay summary log;
- elapsed time;
- whether retry recovered;
- whether a full reboot was needed;
- resulting WAV/BMP when relevant.

Do not call the build hardware-ready until every applicable A–H item passes or
is explicitly waived with a documented reason. This checklist does not approve
the relay for public/security-sensitive use; QR pairing, device credentials,
revocation and production TLS remain a separate release gate.
