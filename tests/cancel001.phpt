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

var_dump($c === $x->connection);
var_dump($c->getResult());
printf("%s\n", $c->errorMessage);
?>
DONE
--EXPECTF--
Test
bool(true)
object(pq\Result)#%d (8) {
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
  ["autoConvert"]=>
  int(255)
}
ERROR:  canceling statement due to user request
DONE
