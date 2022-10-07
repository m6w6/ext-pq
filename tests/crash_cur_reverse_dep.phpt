--TEST--
crash txn reverse dependency from connection
--SKIPIF--
<?php
include "_skipif.inc";
if (version_compare(PHP_VERSION, "8.2", ">="))
	echo "skip PHP_VERSION>=8.2 (dynamic properties deprecated)\n";
?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->t = $c->startTransaction();

?>
===DONE===
--EXPECT--
Test
===DONE===
