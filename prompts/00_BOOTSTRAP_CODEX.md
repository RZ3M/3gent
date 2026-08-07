# Paste this entire prompt into Codex

You are the primary implementation agent for a new open-source project called **3gent**.

3gent is a Nintendo 3DS remote/companion for coding agents. The product and architecture have already been discussed. Your job is **not** to redesign the product from scratch and **not** to build the whole thing in one pass.

## First: load project context

Before changing any files:

1. Read `AGENTS.md`.
2. Read `README.md`.
3. Read every Markdown file directly under `docs/`.
4. Read existing ADRs under `docs/adr/`.
5. Summarize the constraints you believe are locked versus still proposed.
6. Inspect the repository and current local development environment.

Treat `docs/` as the source of truth.

## Important product constraints

These are intentional:

- 3gent is open source.
- Target users are developers with modded Nintendo 3DS-family systems.
- The 3DS is a thin client.
- Heavy work happens on a desktop bridge.
- The architecture is agent-agnostic.
- Voice is the primary input.
- Native 3DS software keyboard is the text fallback.
- Camera photos and stylus sketches are future Capture types.
- Remote access is a core requirement.
- Compatible phone/computer hotspot use is first-class.
- QR pairing is the default pairing UX, with manual fallback.
- We are NOT building our own coding-agent harness.
- We are NOT building a full terminal emulator for the MVP.
- We should eventually support adapters for Codex, Herdr, Claude Code, and other tools.
- For Codex, prefer the official local `codex app-server` rich-client interface over terminal scraping.
- Do not invent custom cryptography.

## Your task right now: Stage 0 only

Bootstrap the repository and implement the **smallest useful feasibility slice**.

The first target milestone is:

> On real 3DS hardware, launch 3gent, open the native software keyboard, type a short message, send it over local Wi-Fi to a tiny local development server, receive a response, and display it on the 3DS.

Do not add the relay.
Do not add production auth.
Do not add Codex integration yet.
Do not add microphone capture until the keyboard + LAN round trip is separated and understandable.
Do not add camera/QR code work yet.
Do not create abstractions for features we have not proven.

## Step A — environment inspection

Check whether the current environment has the current devkitPro 3DS toolchain available.

Use official devkitPro conventions and examples as the primary reference.

If the toolchain is missing:
- do not download random third-party binaries;
- do not silently install system-wide dependencies;
- provide the exact missing prerequisite and official devkitPro source/instruction pointer;
- continue any repo work that does not require the missing tool.

If the toolchain is present:
- identify the relevant environment variables/tools;
- verify that a minimal 3DS example can compile or that the expected compiler/build tools are available.

Do not downgrade current devkitPro packages to make old snippets compile.

## Step B — create only the minimum repository structure

Create the minimum needed for Stage 0, likely something close to:

```text
client-3ds/
  Makefile
  source/
  include/          # only if actually needed

tools/
  dev-server/
```

Do not create empty `bridge/`, `relay/`, and complex protocol packages just for aesthetics unless code genuinely needs them yet.

Add a useful root `.gitignore` if missing.

## Step C — 3DS client: hello + keyboard

Implement a minimal 3DS homebrew application using current devkitPro/libctru conventions.

Requirements:
- clear top and bottom screens;
- display `3gent` and a visible development/build label;
- process the main application loop correctly;
- cleanly exit with an obvious button;
- provide a `Type` action;
- invoke the native software keyboard;
- retrieve entered text;
- display the result locally.

Use the official libctru software keyboard API/example as the reference.

Keep the UI intentionally primitive. This is a feasibility spike.

## Step D — local development server

Create a tiny local echo/test server intended only for Stage 0.

Requirements:
- easy to run on a developer computer;
- bind address and port are explicit/configurable;
- endpoint receives a short text message;
- response clearly proves round-trip behavior;
- logs incoming requests;
- has no production-auth pretensions.

Prefer the simplest implementation that does not lock the future desktop bridge language. A disposable standard-library development utility is fine.

Document exactly how to run it.

## Step E — LAN request from 3DS

After local keyboard capture works, add the smallest network request that proves:

```text
3DS → local dev server → 3DS
```

Requirements:
- endpoint/host is easy to configure for development;
- visible states: connecting / success / error;
- reasonable timeout;
- bounded response;
- no unbounded allocations;
- useful error code/message on screen;
- network resources cleaned up correctly.

For this Stage 0 local-LAN experiment, plain local HTTP is acceptable if clearly marked DEVELOPMENT ONLY.

Do not mistake it for the production remote transport or security design.

## Step F — response rendering

Display the returned text on the 3DS.

If incremental/streamed display is easy without expanding scope, make a tiny simulation or chunked experiment. Otherwise, finish the reliable request/response milestone first and document incremental display as the next spike.

## Step G — documentation

As you work:

- update `docs/RESEARCH.md` with what was actually proven;
- add exact hardware test steps;
- update `README.md` with build/run instructions;
- do not silently change Accepted product decisions;
- if you discover a real architecture question, add it to `docs/DECISIONS.md` as Proposed.

Add a `client-3ds/README.md` containing:
- build prerequisites;
- build command;
- produced artifact;
- installation/run method at a high level;
- hardware verification checklist;
- troubleshooting notes.

Add `tools/dev-server/README.md` with its run command.

## Verification

Run every test/build you reasonably can in the current environment.

At minimum report:
- files created/changed;
- commands run;
- build/test result;
- what still requires physical 3DS hardware;
- assumptions;
- any blocker.

If something cannot be tested because hardware/tooling is unavailable, make that explicit. Do not fake successful hardware results.

## Code quality constraints

- Small and boring beats clever.
- Keep functions focused.
- Bound buffers.
- Check return codes.
- Avoid unnecessary dependencies.
- Avoid premature threading.
- Avoid premature protocol frameworks.
- Do not commit secrets.
- Do not implement home-grown crypto.
- Keep Old 3DS compatibility unless a measured result says otherwise.

## Stop condition

Stop after you have either:

### Success
A buildable Stage 0 implementation for:
- basic 3DS app;
- native keyboard input;
- local LAN text request;
- displayed response;
- documented hardware test procedure;

OR

### Honest blocker
A clean repo bootstrap plus a precise report explaining what tool/hardware dependency prevents the next implementation step.

Do not proceed into Codex adapter, relay, voice transcription, QR pairing, camera capture, or stylus drawing in this first task.

At the end, give me:
1. a concise implementation summary;
2. commands I should run;
3. a physical-3DS test checklist;
4. any proposed decisions that need my answer;
5. the single best next task after I test this on hardware.
