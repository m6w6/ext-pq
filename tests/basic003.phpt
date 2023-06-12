--TEST--
basic functionality
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);

var_dump($c->libraryVersion);
var_dump($c->protocolVersion);
var_dump($c->serverVersion);
?>
DONE
--EXPECTF--
Test
string(%d) "%s"
int(%d)
string(%d) "%s"
DONE
