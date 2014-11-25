--TEST--
crash unbuffered async prepare
--SKIPIF--
<?php 
include "_skipif.inc";
?>
--FILE--
<?php 
echo "Test\n";

include "_setup.inc";

function complete($c) {
	do {
		while ($c->busy) {
			$r = array($c->socket);
			$w = $e = null;
			if (stream_select($r, $w, $e, null)) {
				$c->poll();
			}
		}
	} while ($c->getResult());
}

try {
	$c = new pq\Connection(PQ_DSN);
	$c->unbuffered = true;
	
	$s = $c->prepareAsync("test", "SELECT * from generate_series(1,2)");
	complete($c);
	
	$r = $s->execAsync();
	complete($c);
} catch (Exception $e) {
	echo $e;
}
unset($c);

?>
===DONE===
--EXPECT--
Test
===DONE===