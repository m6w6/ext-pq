--TEST--
async exec params
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->execParamsAsync("SELECT \$1,\$2::int4", array(1,2), array($c->types->byName->int4->oid), function ($res) {
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
object(pq\Result)#%d (6) {
  ["status"]=>
  int(2)
  ["errorMessage"]=>
  string(0) ""
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(2)
  ["affectedRows"]=>
  int(1)
  ["fetchType"]=>
  int(0)
}
DONE