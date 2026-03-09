<?php
declare(strict_types=1);

use Varion\Nghttp2\Events\DataReceived;
use Varion\Nghttp2\Events\GoawayReceived;
use Varion\Nghttp2\Events\HeadersReceived;
use Varion\Nghttp2\ResponseHead;
use Varion\Nghttp2\Session;
use Varion\Nghttp2\StreamEvent;
use Varion\Nghttp2\Events\StreamClosed;

if (!extension_loaded('nghttp2')) {
    fwrite(STDERR, "The nghttp2 extension is not loaded.\n");
    exit(1);
}

$config = parseServerArgs($argv);
$host = $config['address'];
$port = $config['port'];
$tlsEnabled = $config['tls'];
$keyFile = $config['private_key'];
$certFile = $config['cert'];

if ($tlsEnabled) {
    if (!is_file($keyFile) || !is_readable($keyFile)) {
        fwrite(STDERR, "TLS private key file is missing or unreadable: {$keyFile}\n");
        exit(1);
    }

    if (!is_file($certFile) || !is_readable($certFile)) {
        fwrite(STDERR, "TLS certificate file is missing or unreadable: {$certFile}\n");
        exit(1);
    }
}

$scheme = $tlsEnabled ? 'tls' : 'tcp';
$listen = sprintf('%s://%s:%d', $scheme, $host, $port);
$context = $tlsEnabled
    ? stream_context_create([
        'ssl' => [
            'local_cert' => $certFile,
            'local_pk' => $keyFile,
            'crypto_method' => STREAM_CRYPTO_METHOD_TLS_SERVER,
            'disable_compression' => true,
            'alpn_protocols' => 'h2',
        ],
    ])
    : stream_context_create([]);

$server = @stream_socket_server(
    $listen,
    $errno,
    $errstr,
    STREAM_SERVER_BIND | STREAM_SERVER_LISTEN,
    $context
);
if ($server === false) {
    fwrite(STDERR, "Failed to listen on {$listen}: ({$errno}) {$errstr}\n");
    exit(1);
}

if ($tlsEnabled) {
    fwrite(STDERR, "Listening on {$listen} (HTTP/2 over TLS via ALPN)\n");
    fwrite(STDERR, "Try: nghttp -v https://{$host}:{$port}/ --no-verify-peer\n");
    fwrite(STDERR, "Try: curl --http2 -k -v https://{$host}:{$port}/\n");
} else {
    fwrite(STDERR, "Listening on {$listen} (HTTP/2 cleartext h2c prior knowledge)\n");
    fwrite(STDERR, "Try: nghttp -v http://{$host}:{$port}/\n");
    fwrite(STDERR, "Try: curl --http2-prior-knowledge -v http://{$host}:{$port}/\n");
}

while (true) {
    $conn = @stream_socket_accept($server, -1);
    if ($conn === false) {
        continue;
    }

    // Isolate failures per accepted connection so one bad peer does not stop the server.
    try {
        if ($tlsEnabled && !ensureTlsAndAlpn($conn)) {
            continue;
        }

        handleConnection($conn);
    } catch (Throwable $e) {
        fwrite(STDERR, "connection handling error: {$e->getMessage()}\n");
    } finally {
        // Always close the socket on every path (handshake error, read/write error, normal end).
        fclose($conn);
    }
}

function parseServerArgs(array $argv): array
{
    $script = basename($argv[0] ?? 'server.php');
    $args = array_slice($argv, 1);
    $positionals = [];
    $address = '127.0.0.1';

    for ($i = 0; $i < count($args); $i++) {
        $arg = $args[$i];
        if (str_starts_with($arg, '--address=')) {
            $address = substr($arg, strlen('--address='));
            continue;
        }
        if ($arg === '--address') {
            $i++;
            if (!isset($args[$i])) {
                fwrite(STDERR, "--address requires a value\n");
                printUsage($script);
                exit(1);
            }
            $address = $args[$i];
            continue;
        }
        if (str_starts_with($arg, '--')) {
            fwrite(STDERR, "Unknown option: {$arg}\n");
            printUsage($script);
            exit(1);
        }
        $positionals[] = $arg;
    }

    if (count($positionals) !== 1 && count($positionals) !== 3) {
        printUsage($script);
        exit(1);
    }

    $portRaw = $positionals[0];
    if (!ctype_digit($portRaw)) {
        fwrite(STDERR, "Invalid port: {$portRaw}\n");
        exit(1);
    }
    $port = (int) $portRaw;
    if ($port < 1 || $port > 65535) {
        fwrite(STDERR, "Port out of range: {$port}\n");
        exit(1);
    }

    $tlsEnabled = count($positionals) === 3;
    $privateKey = $tlsEnabled ? $positionals[1] : null;
    $cert = $tlsEnabled ? $positionals[2] : null;

    return [
        'address' => $address,
        'port' => $port,
        'tls' => $tlsEnabled,
        'private_key' => $privateKey,
        'cert' => $cert,
    ];
}

function printUsage(string $script): void
{
    fwrite(STDERR, "Usage: php -d extension=modules/nghttp2.so {$script} <PORT> [<PRIVATE_KEY> <CERT>] [--address=<ADDR>]\n");
    fwrite(STDERR, "\n");
    fwrite(STDERR, "Examples:\n");
    fwrite(STDERR, "  TLS (HTTP/2 over TLS):\n");
    fwrite(STDERR, "    php -d extension=modules/nghttp2.so {$script} 8443 ./certs/server.key ./certs/server.crt --address=127.0.0.1\n");
    fwrite(STDERR, "  h2c (HTTP/2 cleartext prior knowledge):\n");
    fwrite(STDERR, "    php -d extension=modules/nghttp2.so {$script} 8080 --address=127.0.0.1\n");
    fwrite(STDERR, "\n");
    fwrite(STDERR, "curl test requests:\n");
    fwrite(STDERR, "  TLS:\n");
    fwrite(STDERR, "    curl --http2 -k -v https://127.0.0.1:8443/\n");
    fwrite(STDERR, "  h2c:\n");
    fwrite(STDERR, "    curl --http2-prior-knowledge -v http://127.0.0.1:8080/\n");
}

function ensureTlsAndAlpn($conn): bool
{
    $meta = stream_get_meta_data($conn);
    if (!isset($meta['crypto']) || !is_array($meta['crypto'])) {
        if (@stream_socket_enable_crypto($conn, true, STREAM_CRYPTO_METHOD_TLS_SERVER) !== true) {
            fwrite(STDERR, "TLS handshake failed\n");
            return false;
        }
        $meta = stream_get_meta_data($conn);
    }

    $crypto = $meta['crypto'] ?? null;
    if (!is_array($crypto)) {
        fwrite(STDERR, "TLS metadata unavailable after handshake\n");
        return false;
    }

    $alpn = $crypto['alpn_protocol']
        ?? $crypto['ssl_alpn_protocol']
        ?? null;

    // TLS can be established without negotiating HTTP/2.
    // Continue only when ALPN selected "h2" so this server speaks a single protocol.
    if ($alpn !== 'h2') {
        fwrite(STDERR, "ALPN negotiation did not select h2 (got: " . ($alpn ?? 'none') . ")\n");
        return false;
    }

    return true;
}

function handleConnection($conn): void
{
    stream_set_timeout($conn, 30);

    $session = new Session(Session::ROLE_SERVER);
    $streamMeta = [];

    // A newly created server session may already have control output queued.
    try {
        flushOutput($session, $conn);
    } catch (Throwable $e) {
        fwrite(STDERR, "socket write failed: {$e->getMessage()}\n");
        return;
    }

    while (!feof($conn)) {
        $bytes = fread($conn, 16384);
        if ($bytes === false) {
            fwrite(STDERR, "socket read failed\n");
            break;
        }

        if ($bytes === '') {
            // Empty reads can happen without immediate failure; treat timeout as fatal, otherwise keep polling.
            $meta = stream_get_meta_data($conn);
            if (!empty($meta['timed_out'])) {
                fwrite(STDERR, "connection timed out\n");
                break;
            }
            continue;
        }

        try {
            $session->receive($bytes);
        } catch (Throwable $e) {
            fwrite(STDERR, "receive error: {$e->getMessage()}\n");
            break;
        }

        $shouldClose = consumeConnectionEvents($session, $streamMeta);
        // Even when GOAWAY was received, flush once so pending control/output frames are not dropped.
        try {
            flushOutput($session, $conn);
        } catch (Throwable $e) {
            // Treat write failure as a connection-scoped error and continue serving other peers.
            fwrite(STDERR, "socket write failed: {$e->getMessage()}\n");
            break;
        }

        if ($shouldClose) {
            break;
        }
    }

    if (count($streamMeta) > 0) {
        // Leftover tracked streams indicate the connection ended before those streams completed.
        fwrite(
            STDERR,
            "connection ended with unfinished streams: " . implode(', ', array_keys($streamMeta)) . "\n"
        );
    }
}

function consumeConnectionEvents(Session $session, array &$streamMeta): bool
{
    // Return true when the connection should close (for example GOAWAY).
    // Return false when frame/event processing can continue.
    while (($event = $session->nextEvent()) !== null) {
        if ($event instanceof GoawayReceived) {
            // This example intentionally simplifies GOAWAY handling by ending the
            // connection loop after one final flush in handleConnection().
            fwrite(STDERR, "peer sent GOAWAY; closing connection\n");
            return true;
        }

        if (!($event instanceof StreamEvent)) {
            continue;
        }

        $sid = (int)$event->streamId;

        if ($event instanceof HeadersReceived) {
            $headers = is_array($event->headers) ? $event->headers : [];
            // A request can carry multiple HEADERS blocks (for example trailers).
            // Keep all blocks for visibility, but preserve the first block as the primary request headers.
            $streamMeta[$sid]['header_blocks'][] = [
                'headers' => $headers,
                'end_stream' => $event->endStream,
            ];
            // Use the first header block as request metadata for response decisions.
            // Later header blocks (trailers) are preserved for inspection only.
            $streamMeta[$sid]['initial_headers'] = $streamMeta[$sid]['initial_headers'] ?? $headers;
            $streamMeta[$sid]['body'] = $streamMeta[$sid]['body'] ?? '';
            $streamMeta[$sid]['responded'] = $streamMeta[$sid]['responded'] ?? false;

            if ($event->endStream && !($streamMeta[$sid]['responded'] ?? false)) {
                $initialHeaders = $streamMeta[$sid]['initial_headers'] ?? [];
                respond($session, $sid, $initialHeaders, $streamMeta[$sid]['body']);
                $streamMeta[$sid]['responded'] = true;
            }
            continue;
        }

        if ($event instanceof DataReceived) {
            if (!isset($streamMeta[$sid]['initial_headers'])) {
                fwrite(STDERR, "stream {$sid} received DATA before request headers; proceeding with minimal fallback\n");
            }
            $streamMeta[$sid]['body'] = ($streamMeta[$sid]['body'] ?? '') . (string)$event->data;
            $streamMeta[$sid]['responded'] = $streamMeta[$sid]['responded'] ?? false;

            if ($event->endStream && !($streamMeta[$sid]['responded'] ?? false)) {
                $initialHeaders = $streamMeta[$sid]['initial_headers'] ?? [];
                respond($session, $sid, $initialHeaders, $streamMeta[$sid]['body']);
                $streamMeta[$sid]['responded'] = true;
            }
            continue;
        }

        if ($event instanceof StreamClosed) {
            $responded = (bool)($streamMeta[$sid]['responded'] ?? false);
            if (!$responded) {
                $bodyLen = strlen((string)($streamMeta[$sid]['body'] ?? ''));
                fwrite(STDERR, "stream {$sid} closed before response (request_body_length={$bodyLen})\n");
            }
            if ($event->errorCode !== 0) {
                fwrite(STDERR, "stream {$sid} closed with error: {$event->errorCode}\n");
            }
            // Remove per-stream state after terminal notification.
            unset($streamMeta[$sid]);
        }
    }

    return false;
}

function respond(Session $session, int $streamId, array $requestHeaders, string $requestBody): void
{
    $method = $requestHeaders[':method'] ?? 'GET';
    $path = $requestHeaders[':path'] ?? '/';

    $payload = [
        'ok' => true,
        'stream_id' => $streamId,
        'method' => $method,
        'path' => $path,
        'request_body_length' => strlen($requestBody),
        'state' => $session->getStreamState($streamId),
        'open_stream_count' => $session->getOpenStreamCount(),
    ];

    $json = json_encode($payload, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
    if (!is_string($json)) {
        $json = '{"ok":false}';
    }

    $headers = [
        'content-type' => 'application/json; charset=utf-8',
        'content-length' => (string)strlen($json),
        'server' => 'ext-nghttp2-example',
    ];

    // Send response headers without ending the stream, then end with the final DATA frame.
    $session->submitResponse($streamId, new ResponseHead(200, $headers, false));
    $session->writeData($streamId, $json, true);
}

function flushOutput(Session $session, $conn): void
{
    foreach ($session->drainOutput() as $chunk) {
        writeAll($conn, $chunk);
    }
}

function writeAll($conn, string $bytes): void
{
    $offset = 0;
    $len = strlen($bytes);

    while ($offset < $len) {
        $chunk = ($offset === 0) ? $bytes : substr($bytes, $offset);
        $written = fwrite($conn, $chunk);
        if ($written === false || $written === 0) {
            throw new RuntimeException('socket write failed');
        }
        $offset += $written;
    }
}
