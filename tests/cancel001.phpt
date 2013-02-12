--TEST--
cancel
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);

$x = new pq\Cancel($c);

$c->execAsync("SELECT pg_sleep(2)");

$x->cancel();

var_dump($c->getResult());

?>
DONE
--EXPECTF--
Test
object(pq\Result)#%d (7) {
  ["status"]=>
  int(7)
  ["statusMessage"]=>
  string(11) "FATAL_ERROR"
  ["errorMessage"]=>
  string(47) "ERROR:  canceling statement due to user request"
  ["numRows"]=>
  int(0)
  ["numCols"]=>
  int(0)
  ["affectedRows"]=>
  int(0)
  ["fetchType"]=>
  int(0)
}
DONE
