--TEST--
basic functionality
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$s = $c->prepare("test1", "SELECT \$1",array($c->types->byName->text->oid));
$r = $s->exec(array("fooo"));

printf("%s\n", $r->errorMessage);
printf("%s\n", $r->fetchCol());
?>
DONE
--EXPECT--
Test

fooo
DONE
