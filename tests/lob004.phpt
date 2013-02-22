--TEST--
large object import/export 
--SKIPIF--
<?php include "_skipif.inc"; ?>
--CLEANUP--
rm lob004.tmp
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = new pq\Transaction($c);

$oid = $t->importLOB(__FILE__);
var_dump($oid);
$t->exportLOB($oid, "lob004.tmp");

var_dump(hash_file("md5",__FILE__)===hash_file("md5","lob004.tmp"));

?>
DONE
--EXPECTF--
Test
int(%d)
bool(true)
DONE

