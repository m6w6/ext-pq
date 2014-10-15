--TEST--
txn properties
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$t = new pq\Transaction(new pq\Connection(PQ_DSN));
var_dump(
	$t->isolation,
	$t->readonly,
	$t->deferrable
);

$t->isolation = pq\Transaction::SERIALIZABLE;
$t->readonly = true;
$t->deferrable = true;
var_dump(
	$t->isolation,
	$t->readonly,
	$t->deferrable
);
?>
DONE
--EXPECTF--
Test
int(0)
bool(false)
bool(false)
int(2)
bool(true)
bool(true)
DONE
