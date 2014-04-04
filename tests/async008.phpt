--TEST--
async cursor
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
$p = $c->declareAsync("mycursor", pq\Cursor::WITH_HOLD,
	"SELECT * FROM generate_series(0,29) s WHERE (s%2)=0");
complete($c);

do {
	$p->fetchAsync(2, function ($r) {
		foreach ($r as $row) {
			foreach ($row as $col) {
				echo "	$col";
			}
			echo "\n";
		}
	});
	complete($p->connection);
	$p->moveAsync(1, function ($r) use(&$keep_going) {
		$keep_going = $r->affectedRows;
	});
	complete($p->connection);
} while ($keep_going);

?>
===DONE===
--EXPECT--
Test
	0
	2
	6
	8
	12
	14
	18
	20
	24
	26
===DONE===
