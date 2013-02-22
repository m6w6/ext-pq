--TEST--
large object stream
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = $c->startTransaction();

$lob = $t->createLOB();
fwrite($lob->stream, file_get_contents(__FILE__));
var_dump(ftell($lob->stream));

fseek($lob->stream, 0, SEEK_SET);
$dat = fread($lob->stream, filesize(__FILE__));
var_dump(hash("md5", $dat)==hash_file("md5", __FILE__));

ftruncate($lob->stream, 5);

$lob = new pq\Lob($t, $lob->oid);
var_dump(fread($lob->stream, 123));

$t->commit();
$t->unlinkLOB($lob->oid);

?>
DONE
--EXPECTF--
Test
int(503)
bool(true)

Warning: ftruncate(): Can't truncate this stream! in %s on line %d
string(123) "<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = $c->startTransaction();

$lob = $t->creat"
DONE

