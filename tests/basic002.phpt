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
var_dump($r->fetchCol($val));
var_dump($val);
?>
DONE
--EXPECT--
Test

bool(true)
string(4) "fooo"
DONE
