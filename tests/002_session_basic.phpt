--TEST--
Session basic receive/drainOutput/nextEvent
--SKIPIF--
<?php
if (!extension_loaded('nghttp2')) {
    echo 'skip nghttp2 extension not loaded';
}
?>
--FILE--
<?php
$client = new Varion\Nghttp2\Session(Varion\Nghttp2\Session::ROLE_CLIENT);
$server = new Varion\Nghttp2\Session(Varion\Nghttp2\Session::ROLE_SERVER);

$out = $client->drainOutput();
var_dump(is_array($out));
var_dump(count($out) > 0);

$consumed = $server->receive($out[0]);
var_dump(is_int($consumed));

$event = $server->nextEvent();
var_dump($event instanceof Varion\Nghttp2\Event || $event === null);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
