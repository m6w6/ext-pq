--TEST--
sql exception
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
try {
	$r = $c->exec("SELECT 1 FROM probably_non_existent_table");
} catch (pq\Exception $e) {
	var_dump($e instanceof pq\Exception\DomainException);
	var_dump($e->getCode() == pq\Exception::SQL);
	var_dump($e->sqlstate);
}
?>
DONE
--EXPECT--
Test
bool(true)
bool(true)
string(5) "42P01"
DONE
