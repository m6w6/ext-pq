--TEST--
transaction
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->exec("DROP TABLE IF EXISTS test CASCADE");
$c->on(pq\Connection::EVENT_NOTICE, function($c, $notice) {
	echo "Got notice: $notice\n";
});
var_dump($c->transactionStatus == pq\Connection::TRANS_IDLE);
$t = new pq\Transaction($c);
var_dump($t->connection->transactionStatus == pq\Connection::TRANS_INTRANS);
$c->exec("DROP TABLE IF EXISTS test");
$c->off(pq\Connection::EVENT_NOTICE);
$c->exec("CREATE TABLE test (id serial, data text)");
$s = $c->prepare("test_insert", "INSERT INTO test (data) VALUES (\$1)", array((new pq\Types($c))["text"]->oid));
$s->exec(array("a"));
$s->exec(array("b"));
$s->exec(array("c"));
$r = $c->exec("SELECT * FROM test");
while ($row = $r->fetchRow(pq\Result::FETCH_OBJECT)) {
	printf("%d => %s\n", $row->id, $row->data);
}
$t->rollback();
var_dump($c->transactionStatus == pq\Connection::TRANS_IDLE);
?>
DONE
--EXPECT--
Test
bool(true)
bool(true)
Got notice: NOTICE:  table "test" does not exist, skipping
1 => a
2 => b
3 => c
bool(true)
DONE
