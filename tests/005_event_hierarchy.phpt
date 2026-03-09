--TEST--
Event hierarchy matches StreamEvent/ConnectionEvent design
--SKIPIF--
<?php
if (!extension_loaded('nghttp2')) {
    echo 'skip nghttp2 extension not loaded';
}
?>
--FILE--
<?php
var_dump(class_exists('Varion\\Nghttp2\\Event'));
var_dump(class_exists('Varion\\Nghttp2\\StreamEvent'));
var_dump(class_exists('Varion\\Nghttp2\\ConnectionEvent'));

$streamEventClasses = [
    'Varion\\Nghttp2\\Events\\HeadersReceived',
    'Varion\\Nghttp2\\Events\\DataReceived',
    'Varion\\Nghttp2\\Events\\StreamClosed',
    'Varion\\Nghttp2\\Events\\StreamReset',
];

foreach ($streamEventClasses as $class) {
    $rc = new ReflectionClass($class);
    var_dump($rc->isSubclassOf('Varion\\Nghttp2\\StreamEvent'));
    var_dump($rc->isSubclassOf('Varion\\Nghttp2\\Event'));
    var_dump($rc->getProperty('streamId')->getDeclaringClass()->getName() === 'Varion\\Nghttp2\\StreamEvent');
}

$connectionEventClasses = [
    'Varion\\Nghttp2\\Events\\GoawayReceived',
    'Varion\\Nghttp2\\Events\\SettingsReceived',
    'Varion\\Nghttp2\\Events\\SettingsAcked',
];

foreach ($connectionEventClasses as $class) {
    $rc = new ReflectionClass($class);
    var_dump($rc->isSubclassOf('Varion\\Nghttp2\\ConnectionEvent'));
    var_dump($rc->isSubclassOf('Varion\\Nghttp2\\Event'));
    var_dump($rc->hasProperty('streamId'));
}
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
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
bool(true)
bool(true)
bool(false)
bool(true)
bool(true)
bool(false)
