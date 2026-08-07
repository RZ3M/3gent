# Stage 0 development server

This is a disposable, standard-library HTTP server for the local 3DS networking
and microphone spikes. It is not the desktop bridge and has no authentication
or production security.

## Run

Python 3.10 or newer is required.

For laptop-only checks:

```sh
python3 tools/dev-server/server.py --host 127.0.0.1 --port 8080
```

For a 3DS on the same trusted LAN:

```sh
python3 tools/dev-server/server.py --host 0.0.0.0 --port 8080
```

Binding to `0.0.0.0` exposes the unauthenticated endpoint to devices that can
reach the computer. Use only disposable test text and stop the server after the
test.

By default, a completed microphone stream is written to
`tools/dev-server/captures/latest.wav`. Use `--capture-dir PATH` to select a
different output directory. A successful stream atomically replaces the prior
`latest.wav`.

Each audio stream is limited to five minutes by default. Override the development
limit with `--max-audio-seconds SECONDS`. The server writes incoming PCM to a
temporary WAV while recording and promotes it to `latest.wav` only after a clean
end-of-stream marker.

## Check from the computer

```sh
curl http://127.0.0.1:8080/health
curl --data 'hello' http://127.0.0.1:8080/echo
```

Expected echo response:

```text
hello from 3gent dev server: hello
```

Incoming echo requests log the source address, byte count, and an escaped preview
limited to 80 characters.

## Test

From the repository root:

```sh
python3 -m unittest discover -s tools/dev-server -p 'test_*.py' -v
```

The tests cover health, UTF-8 echo, the incremental output fixture, unknown
paths, streamed PCM framing/format/size limits, and WAV finalization.

## Interface

- `GET /health` returns a readiness string.
- `POST /echo` accepts `text/plain` UTF-8 and returns a prefixed copy.
- `POST /stream` returns a bounded multi-part fixture over roughly two seconds
  so the 3DS can prove incremental rendering and scrollback.
- `POST /audio/stream` accepts chunked signed little-endian mono PCM16 at
  16,364 Hz and assembles `latest.wav` without buffering the capture in RAM.
- Text requests larger than 4 KiB are rejected.
- Responses include `Content-Length` and close the connection so the Stage 0
  client can parse them simply and predictably.
