--TEST--
RequestHead and ResponseHead can be created
--SKIPIF--
<?php
if (!extension_loaded('nghttp2')) {
    echo 'skip nghttp2 extension not loaded';
}
?>
--FILE--
<?php
$req = new Varion\Nghttp2\RequestHead('GET', 'https', 'example.com', '/', ['accept' => '*/*'], true);
$res = new Varion\Nghttp2\ResponseHead(200, ['content-type' => 'text/plain'], false);

var_dump($req instanceof Varion\Nghttp2\RequestHead);
var_dump($res instanceof Varion\Nghttp2\ResponseHead);
var_dump(is_array($req->headers));
var_dump(is_array($res->headers));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
