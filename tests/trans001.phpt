--TEST--
transaction
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
new pq\Event($c, pq\Event::NOTICE, function($c, $notice) {
	echo "Got notice: $notice";
});
$t = new pq\Transaction($c);
$c->exec("DROP TABLE IF EXISTS test; CREATE TABLE test (id serial, data text)");
$s = $c->prepare("test_insert", "INSERT INTO test (data) VALUES (\$1)", array($c->types->byName->text->oid));
$s->exec(array("a"));
$s->exec(array("b"));
$s->exec(array("c"));
$r = $c->exec("SELECT * FROM test");
while ($row = $r->fetchRow(pq\Result::FETCH_OBJECT)) {
	printf("%d => %s\n", $row->id, $row->data);
}
$t->rollback();
?>
DONE
--EXPECT--
Test
Got notice: NOTICE:  table "test" does not exist, skipping
Got notice: NOTICE:  CREATE TABLE will create implicit sequence "test_id_seq" for serial column "test.id"
1 => a
2 => b
3 => c
DONE
