<?php

declare(strict_types=1);

use Varion\Nghttp2\Session;

$client = new Session(Session::ROLE_CLIENT);
$server = new Session(Session::ROLE_SERVER);

$rounds = 0;
while ($rounds < 4) {
    $rounds++;

    foreach ($client->drainOutput() as $chunk) {
        $server->receive($chunk);
    }
    foreach ($server->drainOutput() as $chunk) {
        $client->receive($chunk);
    }
}

echo "[client events]\n";
while (($event = $client->nextEvent()) !== null) {
    echo get_class($event), PHP_EOL;
}

echo "[server events]\n";
while (($event = $server->nextEvent()) !== null) {
    echo get_class($event), PHP_EOL;
}
