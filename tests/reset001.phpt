--TEST--
connection reset
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->reset();
var_dump($c->status);
$c->reset();
var_dump($c->status);

?>
DONE
--EXPECT--
Test
int(0)
int(0)
DONE
