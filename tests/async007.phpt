--TEST--
async statement
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

function complete($c) {
	do {
		while ($c->busy) {
			$r = array($c->socket);
			$w = $e = null;
			if (stream_select($r, $w, $e, null)) {
				$c->poll();
			}
		}
	} while ($c->getResult());
}

$c = new pq\Connection(PQ_DSN);
$t = new pq\Types($c);
$s = new pq\Statement($c, "test1", "SELECT NOW() - \$1", null, true);
complete($s->connection);

$s->execAsync(array("2012-12-12 12:12:12"));
complete($s->connection);

$s->descAsync(function($r) use ($t) {
	list($typeOid) = $r->desc();
	printf("%s\n", $t[$typeOid]->typname);
});
complete($s->connection);

?>
DONE
--EXPECT--
Test
timestamptz
DONE
