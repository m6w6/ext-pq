--TEST--
Statement properties
--SKIPIF--
<?php
include "_skipif.inc";
?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$n = 'props';
$q = 'SELECT $1, $2, $3';
$t = array(pq\Types::BOOL, pq\Types::INT4, pq\Types::TEXT);

$c = new pq\Connection(PQ_DSN);
$s = new pq\Statement($c, $n, $q, $t);

var_dump($c === $s->connection);
var_dump($n === $s->name);
var_dump($q === $s->query);
var_dump($t === $s->types);

?>
Done
--EXPECT--
Test
bool(true)
bool(true)
bool(true)
bool(true)
Done
