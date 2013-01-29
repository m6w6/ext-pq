--TEST--
large objects
--SKIPIF--
<? php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = $c->startTransaction();

$lob = $t->createLOB();
$lob->write(file_get_contents(__FILE__));
var_dump($lob->tell());
$lob->seek(0, SEEK_SET);
$dat = $lob->read(filesize(__FILE__));
var_dump(hash("md5", $dat));
var_dump(hash_file("md5", __FILE__));
$lob->truncate(5);
$lob = new pq\Lob($t, $lob->oid);
var_dump($lob->read(123));
?>
DONE
--EXPECT--
Test
int(416)
string(32) "d422937493386635bd56b9a9885e7614"
string(32) "d422937493386635bd56b9a9885e7614"
string(5) "<?php"
DONE

