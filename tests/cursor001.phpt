--TEST--
cursor
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$p = $c->declare("mycursor", pq\Cursor::WITH_HOLD,
	"SELECT * FROM generate_series(0,29) s WHERE (s%2)=0");
for ($r = $p->fetch(2); $r->numRows; $p->move(1), $r = $p->fetch(2)) {
	foreach ($r as $row) {
		foreach ($row as $col) {
			echo "	$col";
		}
		echo "\n";
	}
}
try {
	$p = new pq\Cursor($c, "mycursor", pq\Cursor::WITH_HOLD,
		"SELECT * FROM generate_series(0,29) s WHERE (s%2)=0");
} catch (Exception $ex) {
	$p->close();
}
$p = new pq\Cursor($c, "mycursor", pq\Cursor::WITH_HOLD,
	"SELECT * FROM generate_series(0,29) s WHERE (s%2)=0");
for ($r = $p->fetch(2); $r->numRows; $p->move(1), $r = $p->fetch(2)) {
	foreach ($r as $row) {
		foreach ($row as $col) {
			echo "	$col";
		}
		echo "\n";
	}
}
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
