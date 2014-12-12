--TEST--
Deallocate and prepare statement async
--SKIPIF--
<?php include "_skipif.inc"; ?>
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

$c = new pq\Connection(PQ_DSN);
$s = $c->prepareAsync("test1", "SELECT 'test' || \$1");
complete($c);

$r = $s->exec(array("ing"));
$r->fetchCol($d);
var_dump($d);

$s->deallocateAsync();
complete($c);

try {
  $s->exec(array("ing"));
} catch (pq\Exception\BadMethodCallException $e) {
  echo "Caught exception\n";
}

$s->prepareAsync();
complete($c);

$r = $s->exec(array("ing"));
$r->fetchCol($d);
var_dump($d);

?>
DONE
--EXPECT--
Test
string(7) "testing"
Caught exception
string(7) "testing"
DONE
