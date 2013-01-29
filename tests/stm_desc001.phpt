--TEST--
desc statement
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$s = $c->prepare("test1", "SELECT NOW() - \$1");
$r = $s->exec(array("2012-12-12 12:12:12"));
$d = $s->desc();

printf("%s\n", (new pq\Types($c))[$d[0]]->typname);

?>
DONE
--EXPECT--
Test
timestamptz
DONE
