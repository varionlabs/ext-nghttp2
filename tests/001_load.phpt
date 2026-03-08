--TEST--
nghttp2 extension loads and exposes classes
--SKIPIF--
<?php
if (!extension_loaded('nghttp2')) {
    echo 'skip nghttp2 extension not loaded';
}
?>
--FILE--
<?php
var_dump(extension_loaded('nghttp2'));
var_dump(class_exists('Varion\\Nghttp2\\Session'));
var_dump(class_exists('Varion\\Nghttp2\\Event'));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
