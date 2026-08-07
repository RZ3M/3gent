# Stage 0 development server

This is a disposable, standard-library HTTP server for the local 3DS networking
spike. It is not the desktop bridge and has no authentication or production
security.

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

The tests cover health, UTF-8 echo, the incremental stream fixture, unknown
paths, and the 4 KiB request limit.

## Interface

- `GET /health` returns a readiness string.
- `POST /echo` accepts `text/plain` UTF-8 and returns a prefixed copy.
- `POST /stream` returns a bounded multi-part fixture over roughly two seconds
  so the 3DS can prove incremental rendering and scrollback.
- Requests larger than 4 KiB are rejected.
- Responses include `Content-Length` and close the connection so the Stage 0
  client can parse them simply and predictably.
