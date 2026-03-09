# ext-nghttp2

This extension exposes nghttp2 as a Sans-I/O engine for PHP under the `Varion\\Nghttp2` namespace.

## Purpose

- Keep socket I/O and event loops out of the extension, and control the HTTP/2 state machine from PHP
- Use `receive()` / `drainOutput()` / `nextEvent()` as the core API
- Separate transport concerns so integration with ReactPHP or future polling APIs stays straightforward

## Design Principles

- Do not expose callback registration to PHP users; convert callbacks into an internal event queue
- Use `nghttp2_session_mem_recv()` and `nghttp2_session_send()`
- Collect outbound bytes via `drainOutput()`
- Consume protocol events via `nextEvent()`
- Keep introspection minimal in the first release; do not provide a full visualization/debug API yet

## Current Scope

- `Varion\\Nghttp2\\Session`
- `Varion\\Nghttp2\\SessionOptions`
- `Varion\\Nghttp2\\RequestHead`
- `Varion\\Nghttp2\\ResponseHead`
- Event hierarchy: `Varion\\Nghttp2\\Event` (abstract base), `Varion\\Nghttp2\\StreamEvent` (abstract, has `streamId`), `Varion\\Nghttp2\\ConnectionEvent` (abstract)
- Concrete events under `Varion\\Nghttp2\\Events`: stream events (`HeadersReceived`, `DataReceived`, `StreamClosed`, `StreamReset`) and connection events (`GoawayReceived`, `SettingsReceived`, `SettingsAcked`)
- Exception classes (`Exception`, `RuntimeException`, `ProtocolException`)
- Minimal debugging/testing helpers
  - `hasPendingEvents(): bool`
  - `hasPendingOutput(): bool`
  - `getOpenStreamCount(): int`
  - `getStreamState(int $streamId): ?string`

## Event Semantics

- `StreamReset` represents a stream-level forced termination (`RST_STREAM`) and should be treated as an abnormal stream outcome.
- `StreamClosed` is the terminal lifecycle notification for a stream. It is emitted when nghttp2 reports stream closure, regardless of whether the closure was clean or error-driven.
- `StreamClosed::errorCode` carries the close reason from nghttp2 (`0` means `NO_ERROR`; non-zero indicates an error condition).
- Applications that need strict error handling should evaluate both events:
  - `StreamReset` for explicit reset handling and policy decisions.
  - `StreamClosed` for final completion state and close reason inspection.
- The bundled server example intentionally simplifies `GoawayReceived` handling by stopping connection processing after GOAWAY (with one final output flush).

## TODO / Not Implemented

- `SessionOptions::strictValidation` mapping to nghttp2 options
- Advanced header normalization
- Stream list dumps, detailed window-size visibility, frame history, timeline trace
- Large debug visualization APIs such as debug snapshots (can be added in a separate layer later)

## Build

```bash
phpize
./configure --enable-nghttp2
make -j"$(nproc)"
```

## Enable

```bash
php -d extension=$(pwd)/modules/nghttp2.so -m | grep nghttp2
```

## Quick Check

```bash
php -d extension=$(pwd)/modules/nghttp2.so examples/session_basic.php
php -d extension=$(pwd)/modules/nghttp2.so examples/client_preface.php
php -d extension=$(pwd)/modules/nghttp2.so examples/server_preface.php
```

## Server Example

`examples/server-minimal.php` is a minimal event-loop server example for the extension.

### CLI Syntax

```bash
php -d extension=$(pwd)/modules/nghttp2.so examples/server-minimal.php <PORT> [<PRIVATE_KEY> <CERT>] [--address=<ADDR>]
```

- Default address is `127.0.0.1` when `--address` is not specified.
- With `<PRIVATE_KEY> <CERT>`, the server runs in HTTP/2 over TLS mode and requires ALPN `h2`.
- Without key/cert, the server runs in cleartext HTTP/2 (`h2c`, prior knowledge).

### Launch Examples

```bash
# TLS mode (HTTP/2 over TLS)
php -d extension=$(pwd)/modules/nghttp2.so examples/server-minimal.php 8443 ./certs/server.key ./certs/server.crt --address=127.0.0.1

# h2c mode (HTTP/2 cleartext prior knowledge)
php -d extension=$(pwd)/modules/nghttp2.so examples/server-minimal.php 8080 --address=127.0.0.1
```

### curl Test Examples

```bash
# TLS
curl --http2 -k -v https://127.0.0.1:8443/

# h2c
curl --http2-prior-knowledge -v http://127.0.0.1:8080/
```

### Simplifications and Defensive Behavior

- For simplicity, the server example ends connection processing when `GoawayReceived` is observed (after one final output flush).
- Request trailers are preserved for inspection, but response decisions use the first request header block (`initial_headers`).
- The example enforces one response per stream with a `responded` guard flag.
- If `DATA` arrives before request headers, the server logs it as an unexpected order and continues with a minimal fallback path.

## Minimal Example

```php
<?php

use Varion\Nghttp2\Session;

$client = new Session(Session::ROLE_CLIENT);
$server = new Session(Session::ROLE_SERVER);

foreach ($client->drainOutput() as $chunk) {
    $server->receive($chunk);
}

while ($event = $server->nextEvent()) {
    var_dump(get_class($event));
}
```
