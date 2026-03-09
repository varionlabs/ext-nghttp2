<?php
  declare(strict_types=1);

  use Varion\Nghttp2\Events\DataReceived;
  use Varion\Nghttp2\Events\HeadersReceived;
  use Varion\Nghttp2\RequestHead;
  use Varion\Nghttp2\Session;
  use Varion\Nghttp2\StreamEvent;
  use Varion\Nghttp2\Events\StreamClosed;
  use Varion\Nghttp2\Events\StreamReset;

  $host = 'nghttp2.org';
  $port = 443;
  $path = '/httpbin/get';

  $context = stream_context_create([
      'ssl' => [
          // Require verified TLS and explicitly request HTTP/2 via ALPN.
          'verify_peer' => true,
          'verify_peer_name' => true,
          'peer_name' => $host,
          'SNI_enabled' => true,
          'alpn_protocols' => 'h2',
      ],
  ]);

  $socket = stream_socket_client(
      "tls://{$host}:{$port}",
      $errno,
      $errstr,
      10,
      STREAM_CLIENT_CONNECT,
      $context
  );
  if ($socket === false) {
      throw new RuntimeException("connect failed: ($errno) $errstr");
  }
  stream_set_timeout($socket, 10);

  $meta = stream_get_meta_data($socket);
  $alpn = $meta['crypto']['alpn_protocol']
      ?? null;
  // A TLS connection can succeed without selecting HTTP/2.
  // This example continues only when ALPN negotiated "h2".
  if ($alpn !== 'h2') {
      throw new RuntimeException('ALPN negotiation failed (h2 not selected)');
  }

  $session = new Session(Session::ROLE_CLIENT);

  try {
      // Send the client preface and initial SETTINGS.
      flushSessionOutput($session, $socket);

      // Send GET /httpbin/get with no request body (endStream=true).
      $streamId = $session->submitRequest(new RequestHead(
          'GET',
          'https',
          $host,
          $path,
          [
              'accept' => 'application/json',
              'user-agent' => 'ext-nghttp2-example/0.1',
          ],
          true
      ));
      // submitRequest() only enqueues outbound frames; flush to actually send them.
      flushSessionOutput($session, $socket);

      $responseHeaderBlocks = [];
      $responseBody = '';
      $done = false;

      while (!$done && !feof($socket)) {
          $in = fread($socket, 16384);
          if ($in === false) {
              throw new RuntimeException('socket read failed');
          }
          if ($in === '') {
              $info = stream_get_meta_data($socket);
              if (!empty($info['timed_out'])) {
                  throw new RuntimeException('read timeout');
              }
              continue;
          }

          // receive() may create outbound protocol frames (for example ACK/WINDOW_UPDATE),
          // so drain and write output right after feeding inbound bytes.
          $session->receive($in);
          flushSessionOutput($session, $socket);
          // Connection-level events (for example GOAWAY) are intentionally omitted
          // in this minimal example. In production, handle GOAWAY explicitly:
          // stop creating new streams and decide whether this in-flight stream can
          // still complete.
          $done = consumeResponseEvents($session, $streamId, $responseHeaderBlocks, $responseBody);
      }

      if (!$done) {
          throw new RuntimeException('connection closed before target stream completed');
      }
  } finally {
      fclose($socket);
  }

  echo "=== HEADER BLOCKS ===\n";
  var_export($responseHeaderBlocks);
  echo "\n\n=== BODY ===\n";
  echo $responseBody, "\n";

  function writeAll($stream, string $bytes): void
  {
      $offset = 0;
      $len = strlen($bytes);

      // fwrite() may write only part of the buffer; loop until all bytes are sent.
      while ($offset < $len) {
          $chunk = ($offset === 0) ? $bytes : substr($bytes, $offset);
          $written = fwrite($stream, $chunk);
          if ($written === false || $written === 0) {
              throw new RuntimeException('socket write failed');
          }
          $offset += $written;
      }
  }

  function flushSessionOutput(Session $session, $stream): void
  {
      foreach ($session->drainOutput() as $chunk) {
          writeAll($stream, $chunk);
      }
  }

  function consumeResponseEvents(
      Session $session,
      int $targetStreamId,
      array &$responseHeaderBlocks,
      string &$responseBody
  ): bool {
      while (($event = $session->nextEvent()) !== null) {
          if ($event instanceof StreamEvent && $event->streamId !== $targetStreamId) {
              continue;
          }

          if ($event instanceof HeadersReceived) {
              // `end_stream` tells whether this HEADERS block closes the stream.
              // In HTTP/2, HEADERS can appear multiple times (e.g. initial headers and trailers),
              // so keeping this flag per block helps explain where the response actually ended.
              $responseHeaderBlocks[] = [
                  'headers' => is_array($event->headers) ? $event->headers : [],
                  'end_stream' => $event->endStream,
              ];
              continue;
          }

          if ($event instanceof DataReceived) {
              $responseBody .= $event->data;
              continue;
          }

          if ($event instanceof StreamReset) {
              throw new RuntimeException("stream reset: {$event->errorCode}");
          }

          if ($event instanceof StreamClosed) {
              // StreamClosed is the terminal event for this stream.
              // `errorCode === 0` means normal closure (NO_ERROR), while non-zero
              // indicates the stream ended due to an error condition.
              if ($event->errorCode !== 0) {
                  throw new RuntimeException(
                      "stream {$event->streamId} closed with error: {$event->errorCode}"
                  );
              }
              return true;
          }
      }

      return false;
  }
