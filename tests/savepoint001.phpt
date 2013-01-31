--TEST--
savepoints
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = $c->startTransaction();
	$t->savepoint();
		$t->savepoint();
		$t->rollback();
	$t->commit();
$t->rollback();

?>
DONE
--EXPECT--
Test
DONE

