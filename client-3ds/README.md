# 3gent Stage 1 3DS client

This client connects the handheld feasibility work to the TypeScript desktop
bridge and its deterministic fake agent. It is still a local-development build,
not a remotely secure product.

The client currently provides:

- native software-keyboard text capture;
- bounded-memory live microphone streaming using signed PCM16 mono at 16,364 Hz;
- two pre-warmed HTTP/1.1 connections for low-latency commands and audio;
- non-blocking per-frame runtime connection, send, receive, and finalization;
- protocol-v1 commands with unique command IDs;
- bounded adaptive NDJSON event checks with a per-session sequence cursor;
- incremental fake-agent text, status, approval, and interruption UI;
- fixed 2 KiB response storage and held-button scroll navigation.

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
make SERVER_HOST=192.168.1.42 SERVER_PORT=8080
```

Replace `192.168.1.42` with the computer's address. Runtime configuration and
pairing come in later stages. The build produces `3gent.3dsx`, `3gent.smdh`,
`3gent.elf`, and `build/3gent.map`.

From the repository root, install, test, and start the Stage 1 bridge:

```sh
cd bridge
npm ci
npm test
npm start -- --host 0.0.0.0 --port 8080
```

For full request, prompt, acknowledgement, event, and audio-chunk diagnostics,
append `--verbose`. This prints sensitive prompt and agent content and should be
used only for deliberate local debugging.

Binding to `0.0.0.0` exposes an unauthenticated development service to the local
network. Use a trusted LAN and disposable prompts. Do not expose it to the
internet.

## Install and launch

Copy `3gent.3dsx` and `3gent.smdh` into `3ds/3gent/` on the SD card, then launch
3gent through the Homebrew Launcher.

The Stage 1 controls are:

- `A`: open the native keyboard and send text to the fake agent;
- `X`: start a fake approval demo, or approve once when one is pending;
- `B`: interrupt an active turn, or decline a pending approval;
- hold `R`: stream microphone audio, then send it on release;
- D-pad/Circle Pad Up/Down: scroll the response, including held repeat;
- `START`: exit.

Successful audio is saved as `bridge/data/latest.wav`. The fake adapter does not
transcribe it yet; it generates a mock voice-capture response so the full event
path can be tested.

## Physical Stage 1 verification checklist

Record the hardware model, host OS, tool versions, and displayed timings.

1. Start the TypeScript bridge and confirm it prints its listening address.
2. Launch `0.1.1-stage1` through the Homebrew Launcher.
3. Confirm startup reports `warm links: 2/2`, then shows agent state `idle`.
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
14. Leave the app idle for at least eleven minutes, then send another prompt and
    start a short audio capture. Record whether both connections recover without
    restarting the app.
15. With a scrollable response on screen, stop the bridge. While an event check
    and then a user command fail in the background, hold Up/Down and confirm the
    screen and held-repeat navigation never freeze for the five-second network
    timeout.
16. Restart the bridge and confirm the app reports a session resync, then sends a
    new prompt and displays the complete fake-agent response without restarting
    the app.
17. Stop the bridge during a short microphone capture, release `R`, and confirm
    the finalizing UI remains live until it reports a bounded stream error.
18. Close and reopen the shell during an active turn and record the resulting
    stop, error, or resume behavior.
19. Press `START` and confirm a clean return to the Homebrew Launcher.

Do not mark the Stage 1 handheld slice complete until steps 1–13 pass. The
long-idle, restart, and shell behavior are explicit reliability follow-ups even
if the main vertical slice succeeds.

The current development client checks for agent events about every 100 ms while
working, 250 ms while an approval is pending, once per second while idle, and
with one-to-ten-second retry backoff after failure. These are bridge reads, not
phantom button events. Local push delivery is the next transport experiment.

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
