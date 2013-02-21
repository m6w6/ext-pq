--TEST--
persistent handles
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

for ($i=0; $i<100; ++$i) {
	$c = new pq\Connection(PQ_DSN, pq\Connection::PERSISTENT);

	if ($i % 2) {
		$t = new pq\Transaction($c);
		$c->listen("chan", function($chan, $msg) {
			// dummy
		});
		$c->on(pq\Connection::EVENT_RESULT, function($c, $res) {
		});
	}
	
	if (!($i%10)) gc_collect_cycles();

	$c->exec("");
}
var_dump(raphf\stat_persistent_handles()->{"pq\\Connection"});
?>
DONE
--EXPECTF--
Test
array(1) {
  ["%S"]=>
  array(2) {
    ["used"]=>
    int(1)
    ["free"]=>
    int(2)
  }
}
DONE
