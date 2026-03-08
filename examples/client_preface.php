<?php

declare(strict_types=1);

use Varion\Nghttp2\Session;

$client = new Session(Session::ROLE_CLIENT);

$output = $client->drainOutput();
echo "client output chunks: ", count($output), PHP_EOL;
foreach ($output as $i => $chunk) {
    echo sprintf("chunk[%d] len=%d", $i, strlen($chunk)), PHP_EOL;
}

while (($event = $client->nextEvent()) !== null) {
    echo get_class($event), PHP_EOL;
}
