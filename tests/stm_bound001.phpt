--TEST--
statement w/ bound vars
--SKIPIF--
<?php
include "_skipif.inc";
?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$s = new pq\Statement($c, "bound1", "SELECT \$1::text, \$2::text, \$3::text");
$s->bind(0, $_1);
$s->bind(1, $_2);
$s->bind(2, $_3);
$r = $s->exec();
var_dump($r->fetchAll());
$_1 = "\$1";
$_2 = "\$2";
$_3 = "\$3";
$r = $s->exec();
var_dump($r->fetchAll());
?>
Done
--EXPECT--
Test
array(1) {
  [0]=>
  array(3) {
    [0]=>
    NULL
    [1]=>
    NULL
    [2]=>
    NULL
  }
}
array(1) {
  [0]=>
  array(3) {
    [0]=>
    string(2) "$1"
    [1]=>
    string(2) "$2"
    [2]=>
    string(2) "$3"
  }
}
Done
