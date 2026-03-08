<?php

declare(strict_types=1);

use Varion\Nghttp2\Session;

$client = new Session(Session::ROLE_CLIENT);
$server = new Session(Session::ROLE_SERVER);

foreach ($client->drainOutput() as $chunk) {
    $server->receive($chunk);
}

echo "server events after receiving client preface:\n";
while (($event = $server->nextEvent()) !== null) {
    echo get_class($event), PHP_EOL;
}

echo "server output chunks: ", count($server->drainOutput()), PHP_EOL;
