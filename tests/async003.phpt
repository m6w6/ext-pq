--TEST--
async exec
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->execAsync("SELECT 1+2+3; SELECT 2,3,4", function ($res) {
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
object(pq\Result)#%d (8) {
  ["status"]=>
  int(2)
  ["statusMessage"]=>
  string(9) "TUPLES_OK"
  ["errorMessage"]=>
  string(0) ""
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(1)
  ["fetchType"]=>
  int(0)
  ["autoConvert"]=>
  int(255)
}
object(pq\Result)#%d (8) {
  ["status"]=>
  int(2)
  ["statusMessage"]=>
  string(9) "TUPLES_OK"
  ["errorMessage"]=>
  string(0) ""
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(3)
  ["affectedRows"]=>
  int(1)
  ["fetchType"]=>
  int(0)
  ["autoConvert"]=>
  int(255)
}
DONE
