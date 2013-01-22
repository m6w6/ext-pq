--TEST--
async prepared statement
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

function complete($s) {
	do {
		while ($s->connection->busy) {
			$r = array($s->connection->socket);
			$w = $e = null;
			if (stream_select($r, $w, $e, null)) {
				$s->connection->poll();
			}
		}
	} while ($s->connection->getResult());
}

$c = new pq\Connection(PQ_DSN);
$s = $c->prepareAsync("test", "SELECT \$1,\$2::int4", array($c->types->byName->int4->oid));

complete($s);

$s->execAsync(array(1,2), function ($res) {
	var_dump($res);
});

complete($s);

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