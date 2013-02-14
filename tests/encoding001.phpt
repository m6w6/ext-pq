--TEST--
encoding
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
var_dump($c->encoding);
$c->encoding = "utf8";
var_dump($c->encoding);
var_dump($c->exec("SELECT 'ßüpä…'")->fetchCol());
$tmp = 12345;
$c->encoding = $tmp;
var_dump($c->encoding);
?>
DONE
--EXPECTF--
Test
string(%d) "%s"
string(4) "UTF8"
string(10) "ßüpä…"

Notice: Unrecognized encoding '12345' in %s on line %d
string(4) "UTF8"
DONE
