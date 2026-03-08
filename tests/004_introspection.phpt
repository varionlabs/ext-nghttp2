--TEST--
Session introspection APIs
--SKIPIF--
<?php
if (!extension_loaded('nghttp2')) {
    echo 'skip nghttp2 extension not loaded';
}
?>
--FILE--
<?php
use Varion\Nghttp2\RequestHead;
use Varion\Nghttp2\Session;

$client = new Session(Session::ROLE_CLIENT);
$server = new Session(Session::ROLE_SERVER);

var_dump(is_bool($client->hasPendingEvents()));
var_dump(is_bool($client->hasPendingOutput()));
var_dump(is_int($client->getOpenStreamCount()));

$state = $client->getStreamState(1);
var_dump(is_string($state) || $state === null);

foreach ($client->drainOutput() as $chunk) {
    $server->receive($chunk);
}
foreach ($server->drainOutput() as $chunk) {
    $client->receive($chunk);
}

$streamId = $client->submitRequest(new RequestHead('GET', 'https', 'example.com', '/', [], false));
$beforeEnd = $client->getStreamState($streamId);
$countBeforeEnd = $client->getOpenStreamCount();

$client->endStream($streamId);
$afterEnd = $client->getStreamState($streamId);

var_dump(is_string($beforeEnd) || $beforeEnd === null);
var_dump(is_int($countBeforeEnd));
var_dump(is_string($afterEnd) || $afterEnd === null);
var_dump(($beforeEnd !== $afterEnd) || $afterEnd === 'half_closed_local' || $afterEnd === 'closed');

$client->resetStream($streamId);
$afterReset = $client->getStreamState($streamId);
var_dump($afterReset === 'closed' || $afterReset === null || $afterReset === 'half_closed_local' || $afterReset === 'half_closed_remote');
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
