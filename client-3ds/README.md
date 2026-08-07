# 3gent Stage 0 3DS client

This client proves the first three 3DS boundaries:

1. launch a basic top/bottom-screen homebrew app;
2. retrieve text from the native software keyboard;
3. send that text to a LAN development server and display its response.

This is a development spike, not the production bridge or protocol. It has not
yet been verified on physical hardware.

## Prerequisites

Install the current official devkitPro toolchain. On macOS, follow the
[devkitPro getting-started instructions](https://devkitpro.org/wiki/Getting_Started):

1. Install Apple's command-line tools with `xcode-select --install` if needed.
2. Install the official devkitPro pacman package for macOS.
3. Restart the Mac or reload the devkitPro environment setup.
4. Install the supported 3DS package group:

   ```sh
   sudo dkp-pacman -Syu
   sudo dkp-pacman -S 3ds-dev
   ```

The build expects `DEVKITPRO`, `DEVKITARM`, `arm-none-eabi-gcc`, and libctru to
come from that installation. Do not downgrade the toolchain for this project.

## Configure and build

First find the LAN IPv4 address of the computer running the development server.
The 3DS must be able to reach this address. Then build with that numeric address:

```sh
cd client-3ds
make SERVER_HOST=192.168.1.42 SERVER_PORT=8080
```

Replace `192.168.1.42` with the development computer's address. Stage 0 accepts
a numeric IPv4 address only; runtime configuration and discovery are intentionally
deferred.

Produced files:

- `3gent.3dsx` — Homebrew Launcher application;
- `3gent.smdh` — application metadata;
- `3gent.elf` and `3gent.map` — development outputs.

Run `make clean` to remove generated files.

## Start the development server

From the repository root:

```sh
python3 tools/dev-server/server.py --host 0.0.0.0 --port 8080
```

This unauthenticated server is for a trusted development LAN and disposable test
messages only. See [the server README](../tools/dev-server/README.md) for checks
and troubleshooting.

## Install and launch

Copy `3gent.3dsx` and `3gent.smdh` to a `3ds/3gent/` directory on the SD card,
then launch 3gent through the Homebrew Launcher. Exact copying steps depend on
the user's existing homebrew setup.

## Physical hardware verification checklist

Record the hardware model, host OS, devkitPro package versions, and network setup
with the result.

1. Start the development server and confirm that `/health` responds.
2. Launch 3gent from the Homebrew Launcher.
3. Confirm that both screens clear and render without corruption.
4. Confirm that the top screen shows `3gent`, `Stage 0A-C`, and version
   `0.0.1-stage0`.
5. Confirm that the bottom screen shows the configured IP address and reports the
   network service as ready.
6. Press `A`; confirm that the native software keyboard opens.
7. Choose Cancel; confirm that the app returns safely and reports cancellation.
8. Press `A` again, type `hello`, and choose Send.
9. Confirm that `Connecting...` is visibly shown.
10. Confirm that the server logs the request from the 3DS.
11. Confirm that the top screen shows `Success`, the entered text, and
    `hello from 3gent dev server: hello`.
12. Stop the server and send again; confirm that an error or timeout appears
    within roughly five seconds.
13. Restart the server and confirm that a later request succeeds without
    restarting the 3DS app.
14. Press `START`; confirm that the app exits cleanly to the Homebrew Launcher.

Do not mark `R-001`, `R-002`, or the 3DS portion of `R-003` proven until these
steps have been performed on hardware.

## Troubleshooting

- `DEVKITARM is not set`: install the official `3ds-dev` package group and restart
  the shell or Mac so the devkitPro environment is loaded.
- `connect timed out`: verify the compiled IP, server bind address, computer
  firewall, and that both devices can communicate on the same LAN.
- `connect failed (... Connection refused)`: start the server on the compiled
  port and verify no firewall is rejecting it.
- `server returned HTTP 404`: use the supplied Stage 0 server; the client sends
  `POST /echo`.
- `response exceeded the bounded buffer`: the client intentionally refuses an
  HTTP response larger than its fixed development buffer.
