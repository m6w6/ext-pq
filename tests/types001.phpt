--TEST--
types functionality
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = new pq\Types($c);
var_dump($t->connection === $c);
var_dump(isset($t["int4"]), empty($t["int4"]));
var_dump(isset($t["whatthahell"]), empty($t["whatthahell"]));

var_dump(isset($t[25]), empty($t[25]));
var_dump(isset($t[0]), empty($t[0]));
?>
DONE
--EXPECT--
Test
bool(true)
bool(true)
bool(false)
bool(false)
bool(true)
bool(true)
bool(false)
bool(false)
bool(true)
DONE
