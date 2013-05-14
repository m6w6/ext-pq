--TEST--
ext info
--SKIPIF--
<?php
include "_skipif.inc";
?>
--FILE--
<?php
echo "Test\n";
$e = new ReflectionExtension("pq");
$e->info();
?>
Done
--EXPECTF--
Test

pq

PQ Support => enabled
Extension Version => %s

Used Library => Version
libpq => %s
Done
