--TEST--
large objects
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = $c->startTransaction();

$lob = $t->createLOB();

var_dump($lob->transaction === $t);

$lob->write(file_get_contents(__FILE__));
var_dump($lob->tell());

$lob->seek(0, SEEK_SET);
$dat = $lob->read(filesize(__FILE__));
var_dump(hash("md5", $dat)==hash_file("md5", __FILE__));

$lob->truncate(5);

$lob = new pq\Lob($t, $lob->oid);
var_dump($lob->read(123));

$t->commit();
$t->unlinkLOB($lob->oid);

?>
DONE
--EXPECTF--
Test
bool(true)
int(489)
bool(true)
string(5) "%c?php"
DONE

