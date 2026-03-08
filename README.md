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
- `Varion\\Nghttp2\\Event` and minimal event classes
- Exception classes (`Exception`, `RuntimeException`, `ProtocolException`)
- Minimal debugging/testing helpers
  - `hasPendingEvents(): bool`
  - `hasPendingOutput(): bool`
  - `getOpenStreamCount(): int`
  - `getStreamState(int $streamId): ?string`

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
