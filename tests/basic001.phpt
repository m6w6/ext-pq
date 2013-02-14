--TEST--
basic functionality
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$con = new pq\Connection(PQ_DSN);
$res = $con->exec("SELECT 1 as one, 2 as two from generate_series(1,2)");

var_dump($res->status == pq\Result::TUPLES_OK);
var_dump($res->numRows);
var_dump($res->numCols);
var_dump(count($res) == $res->count(), $res->numRows == count($res));

foreach ($res as $rowNum => $rowData) {
	printf("%d.0 => %d\n", $rowNum, $rowData[0]);
	printf("%d.1 => %d\n", $rowNum, $rowData[1]);
}
$res->fetchType = pq\Result::FETCH_ASSOC;
foreach ($res as $rowNum => $rowData) {
	printf("%d.0 => %d\n", $rowNum, $rowData["one"]);
	printf("%d.1 => %d\n", $rowNum, $rowData["two"]);
}
$res->fetchType = pq\Result::FETCH_OBJECT;
foreach ($res as $rowNum => $rowData) {
	printf("%d.0 => %d\n", $rowNum, $rowData->one);
	printf("%d.1 => %d\n", $rowNum, $rowData->two);
}
?>
DONE
--EXPECT--
Test
bool(true)
int(2)
int(2)
bool(true)
bool(true)
0.0 => 1
0.1 => 2
1.0 => 1
1.1 => 2
0.0 => 1
0.1 => 2
1.0 => 1
1.1 => 2
0.0 => 1
0.1 => 2
1.0 => 1
1.1 => 2
DONE
