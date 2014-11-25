--TEST--
crash result iterator
--SKIPIF--
<?php
include "_skipif.inc";
?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$conn = new pq\Connection(PQ_DSN);

$sql = "
	SELECT id
	FROM generate_series(1,3) id
	ORDER BY id ASC
	LIMIT 5
";

foreach ($conn->exec($sql) as $row) {
	var_dump($row);
}
?>
===DONE===
--EXPECT--
Test
array(1) {
  [0]=>
  int(1)
}
array(1) {
  [0]=>
  int(2)
}
array(1) {
  [0]=>
  int(3)
}
===DONE===
