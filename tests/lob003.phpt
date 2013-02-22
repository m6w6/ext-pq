--TEST--
large object closing stream 
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = $c->startTransaction();

$lob = $t->createLOB();
var_dump($lob->stream);
var_dump($lob->stream);
fclose($lob->stream); // bad boy!
var_dump($lob->stream);
var_dump(fread($lob->stream, 5));
$lob = null;
?>
DONE
--EXPECTF--
Test
resource(%d) of type (stream)
resource(%d) of type (stream)

Warning: fclose(): %d is not a valid stream resource in %s on line %d
resource(%d) of type (stream)
string(0) ""
DONE

