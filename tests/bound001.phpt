--TEST--
fetch bound
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$r = $c->exec("select 1*a,2*a,3*a from generate_series(2,3) a");
$r->bind(0, $a);
$r->bind(1, $b);
$r->bind(2, $c);
while ($s = $r->fetchBound()) {
	var_dump($s,$a,$b,$c);
}
?>
DONE
--EXPECT--
Test
array(3) {
  [0]=>
  &string(1) "2"
  [1]=>
  &string(1) "4"
  [2]=>
  &string(1) "6"
}
string(1) "2"
string(1) "4"
string(1) "6"
array(3) {
  [0]=>
  &string(1) "3"
  [1]=>
  &string(1) "6"
  [2]=>
  &string(1) "9"
}
string(1) "3"
string(1) "6"
string(1) "9"
DONE
