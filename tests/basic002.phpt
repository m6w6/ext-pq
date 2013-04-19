--TEST--
basic functionality
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = new pq\Types($c);
$s = $c->prepare("test1", "SELECT \$1",array($t["text"]->oid));
$r = $s->exec(array("fooo"));

printf("%s\n", $r->errorMessage);
$r->fetchCol(0, $val);
printf("%s\n", $val);
?>
DONE
--EXPECT--
Test

fooo
DONE
