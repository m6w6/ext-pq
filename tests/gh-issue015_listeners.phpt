--TEST--
restore listeners on reset
--SKIPIF--
<?php
include "_skipif.inc";
?>
--INI--
date.timezone=UTC
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);

$c->listen("notify", function($channel, $message) {
	printf("%s: %s\n", $channel, $message);
});
$c->on(pq\Connection::EVENT_RESET, function($conn) {
	printf("Connection was reset\n");
});
$c->notify("notify", "Gotcha!");
$c->resetAsync();

// wait until the stream becomes writable
$w = array($c->socket);
$r = $e = null;

if (stream_select($r, $w, $e, null)) {

	// loop until the connection is established
	while (true) {

		switch ($c->poll()) {

			case pq\Connection::POLLING_READING:
				// we should wait for the stream to be read-ready
				$r = array($c->socket);
				stream_select($r, $w, $e, NULL);
				break;

			case pq\Connection::POLLING_WRITING:
				// we should wait for the stream to be write-ready
				$w = array($c->socket);
				$r = $e = null;
				stream_select($r, $w, $e, null);
				break;

			case pq\Connection::POLLING_FAILED:
				printf("Connection failed: %s\n", $c->errorMessage);
				break 2;

			case pq\Connection::POLLING_OK:
				printf("Connection completed\n");
				break 2;
		}
	}
}
$c->notify("notify", "Do you miss me?");
$c->exec("");
?>
===DONE===
--EXPECT--
Test
notify: Gotcha!
Connection was reset
Connection completed
notify: Do you miss me?
===DONE===