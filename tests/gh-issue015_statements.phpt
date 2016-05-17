--TEST--
restore statements on reset
--SKIPIF--
<?php
include "_skipif.inc";
?>
--INI--
date.timezone=UTC
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);

$s = $c->prepare("test", "SELECT 1");
$c->on(pq\Connection::EVENT_RESET, function($conn) {
	printf("Connection was reset\n");
});

var_dump($s->exec()->fetchRow());

$c->reset();

// Fatal error: Uncaught exception 'pq\Exception\DomainException' with message 'ERROR:  prepared statement "test" does not exist'
var_dump($s->exec()->fetchRow());

?>
===DONE===
--EXPECT--
Test
array(1) {
  [0]=>
  int(1)
}
Connection was reset
array(1) {
  [0]=>
  int(1)
}
===DONE===