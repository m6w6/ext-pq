--TEST--
async unbuffered exec
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->unbuffered = true;
$c->execAsync("SELECT a FROM generate_series(1,3) a", function ($res) {
	var_dump($res);
});
do {
	while ($c->busy) {
		$r = array($c->socket);
		$w = $e = null;
		if (stream_select($r, $w, $e, null)) {
			$c->poll();
		}
	}
} while ($c->getResult());

?>
DONE
--EXPECTF--
Test
object(pq\Result)#%d (7) {
  ["status"]=>
  int(9)
  ["statusMessage"]=>
  string(12) "SINGLE_TUPLE"
  ["errorMessage"]=>
  string(0) ""
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(0)
  ["fetchType"]=>
  int(0)
}
object(pq\Result)#%d (7) {
  ["status"]=>
  int(9)
  ["statusMessage"]=>
  string(12) "SINGLE_TUPLE"
  ["errorMessage"]=>
  string(0) ""
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(0)
  ["fetchType"]=>
  int(0)
}
object(pq\Result)#%d (7) {
  ["status"]=>
  int(9)
  ["statusMessage"]=>
  string(12) "SINGLE_TUPLE"
  ["errorMessage"]=>
  string(0) ""
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(0)
  ["fetchType"]=>
  int(0)
}
object(pq\Result)#%d (7) {
  ["status"]=>
  int(2)
  ["statusMessage"]=>
  string(9) "TUPLES_OK"
  ["errorMessage"]=>
  string(0) ""
  ["numRows"]=>
  int(0)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(3)
  ["fetchType"]=>
  int(0)
}
DONE
