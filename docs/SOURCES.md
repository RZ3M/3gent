# 3gent — Primary Research Sources

**Last checked:** 2026-08-08

Prefer these primary/official sources when implementation details conflict with memory, old tutorials, or random snippets.

## devkitPro / 3DS homebrew

### libctru
https://github.com/devkitPro/libctru

Notes:
- libctru is the user-mode 3DS homebrew library.
- devkitARM is its officially supported cross-compiling toolchain.
- devkitPro recommends staying current rather than downgrading around old code.

### Official 3DS examples
https://github.com/devkitPro/3ds-examples

Relevant example categories include:
- audio
- camera
- graphics
- input
- network
- templates

### Software keyboard API
https://libctru.devkitpro.org/swkbd_8h.html

Relevant functions include `swkbdInit` and `swkbdInputText`.

### devkitPro 3DS curl package definition
https://github.com/devkitPro/pacman-packages/blob/master/3ds/curl/PKGBUILD

Version-sensitive findings checked on 2026-08-08:
- package version 8.4.0-1;
- threaded resolver and pthreads disabled;
- no WebSocket enable flag;
- SD-card CA bundle path configured.

### devkitPro 3DS Mbed TLS package definition
https://github.com/devkitPro/pacman-packages/blob/master/3ds/mbedtls/PKGBUILD

Version-sensitive finding checked on 2026-08-08: package version 2.28.8-1.

## Secure transport upstreams

### curl 8.11.0 release notes
https://curl.se/ch/8.11.0.html

WebSocket support became official/non-experimental in curl 8.11.0. Do not infer
production WebSocket support from the older 3DS curl package merely because its
headers expose some related API.

### Mbed TLS maintained branches
https://github.com/Mbed-TLS/mbedtls/blob/development/BRANCHES.md

As checked on 2026-08-08, upstream lists 3.6 and 4.1 as maintained LTS branches.
Recheck this at implementation time.

## Nintendo network compatibility

### devkitPro camera image example
https://github.com/devkitPro/3ds-examples/tree/master/camera/image

The Stage 6 spike follows the current official libctru example's camera service,
RGB565, transfer-size, receive-event, shutter and cleanup sequence, narrowed to
one 400×240 outer-camera image.

### Compatible wireless modes/security
https://en-americas-support.nintendo.com/app/answers/detail/a_id/498/p/897/c/871

Key fact:
- 3DS-family systems use 2.4 GHz 802.11b/g and support several WEP/WPA/WPA2 personal configurations depending on model.

### Hotspot/captive portal troubleshooting
https://en-americas-support.nintendo.com/app/answers/detail/a_id/5896/

Nintendo documents that some sign-in/terms networks can require the 3DS browser; hotspot behavior is not universally reliable.

### Internet setup
https://en-americas-support.nintendo.com/app/answers/detail/a_id/499/

## OpenAI Codex

### Codex and AGENTS.md
https://openai.com/index/introducing-codex/

Codex supports `AGENTS.md` files for repository instructions.

### Harness engineering / docs as source of truth
https://openai.com/index/harness-engineering/

Useful repository pattern:
- concise `AGENTS.md`;
- deeper structured docs as the system of record.

### Codex app-server README
https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md

Important current behavior:
- local rich-client interface;
- JSON-RPC-style protocol;
- stdio transport;
- threads/turns/items;
- streaming events;
- approvals;
- diff updates;
- schema generation commands;
- direct WebSocket listener is documented as experimental/unsupported, so do not make it the production dependency.

### OpenAI audio transcription API
https://platform.openai.com/docs/api-reference/audio/createTranscription

The optional hosted Stage 3 backend sends a WAV multipart upload to
`/v1/audio/transcriptions`. The default `gpt-4o-mini-transcribe` model and WAV
input support were rechecked against the official API reference on 2026-08-08.
The API key remains on the desktop bridge.

## Herdr

https://herdr.dev/
https://herdr.dev/docs/agent-automation/
https://herdr.dev/docs/agents/

Useful for:
- persistent agent-aware terminals;
- working/blocked/idle states;
- agent prompting/reading/waiting;
- optional 3gent adapter.

## Moshi

https://getmoshi.app/docs/introduction
https://getmoshi.app/docs/terminal-sessions

Useful for:
- remote coding-agent companion UX;
- voice/approval/diff ideas;
- reconnect-oriented mobile workflows.

## Research rule

Before copying code or install steps from third-party tutorials:
1. check the current official source;
2. verify against the current toolchain;
3. record any version-sensitive workaround in `RESEARCH.md`.
