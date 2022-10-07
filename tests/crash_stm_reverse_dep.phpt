--TEST--
crash stm reverse dependency from connection
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
$c->s = $c->prepare("test", "SELECT 1");

?>
===DONE===
--EXPECT--
Test
===DONE===
