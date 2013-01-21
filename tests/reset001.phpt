--TEST--
connection reset
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
var_dump($c->reset());
var_dump($c->reset());

?>
DONE
--EXPECT--
Test
bool(true)
bool(true)
DONE
