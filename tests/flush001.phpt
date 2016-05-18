--TEST--
flush
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->nonblocking = true;
var_dump($c->nonblocking);
$c->execAsync("SELECT '".str_repeat("a", 6e7)."'", function($r) {
	$r->fetchCol($s);
	var_dump(strlen($s));
});
var_dump($flushed = $c->flush());
do {
	while (!$flushed || $c->busy) {
		$r = $c->busy ? [$c->socket] : null;
		$w = !$flushed ?[$c->socket] : null; 
		
		if (stream_select($r, $w, $e, null)) {
			if ($r) {
				printf("P%d", $c->poll());
			}
			if ($w) {
				printf("F%d", $flushed = $c->flush());
			}
		}
	}
	echo "\n";
} while ($c->getResult());
?>
===DONE===
--EXPECTF--
Test
bool(true)
bool(%s)
%r(F0)*(F1)*(P3)+%r
int(60000000)

===DONE===
