--TEST--
notify
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$consumer = new pq\Connection(PQ_DSN);
$consumer->listen("test", function($channel, $message, $pid) {
	printf("%s(%d): %s\n", $channel, $pid, $message);
});

$producer = new pq\Connection(PQ_DSN);
$producer->notify("test", "this is a test");

$consumer->exec("select 1");

$producer->notify("test", "this is an async test");

$r = array($consumer->socket);
$w = null; $e = null;
var_dump(stream_select($r, $w, $e, 0));
$consumer->poll();

$producer->notify("other", "this should not show up");

var_dump(stream_select($r, $w, $e, 0));
$consumer->poll();

$producer->notify("test", "just to be sure");

$r = array($consumer->socket);
$w = null; $e = null;
var_dump(stream_select($r, $w, $e, 0));
$consumer->poll();

$consumer->unlisten("test");

$producer->notify("test", "this shouldn't show up either");

$r = array($consumer->socket);
$w = null; $e = null;
var_dump(stream_select($r, $w, $e, 0));
$consumer->poll();

?>
DONE
--EXPECTF--
Test
test(%d): this is a test
int(1)
test(%d): this is an async test
int(0)
int(1)
test(%d): just to be sure
int(0)
DONE
