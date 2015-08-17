--TEST--
fetch bound
--SKIPIF--
<?php 
include "_skipif.inc";
?>
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
  int(2)
  [1]=>
  int(4)
  [2]=>
  int(6)
}
int(2)
int(4)
int(6)
array(3) {
  [0]=>
  int(3)
  [1]=>
  int(6)
  [2]=>
  int(9)
}
int(3)
int(6)
int(9)
DONE
