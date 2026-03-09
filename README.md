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
