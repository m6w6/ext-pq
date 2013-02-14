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
new pq\Event($c, pq\Event::RESET, function ($c) { print "RESET!\n"; });
$c->reset();
var_dump($c->status);

?>
DONE
--EXPECT--
Test
int(0)
RESET!
int(0)
DONE
