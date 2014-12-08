--TEST--
Deallocated and prepare statement
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$s = $c->prepare("test1", "SELECT 'test' || \$1");

$r = $s->exec(array("ing"));
$r->fetchCol($d);
var_dump($d);

$s->deallocate();

try {
  $s->exec(array("ing"));
} catch (pq\Exception\BadMethodCallException $e) {
  echo "Caught exception\n";
}

$s->prepare();

$r = $s->exec(array("ing"));
$r->fetchCol($d);
var_dump($d);

?>
DONE
--EXPECT--
Test
string(7) "testing"
Caught exception
string(7) "testing"
DONE
