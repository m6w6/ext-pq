--TEST--
async connect
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN, true);
$s = array($c->status);
echo "W";
$w = array($c->socket);
$r = $e = null;
stream_select($r, $w, $e, null);
while (true) {
	$s[] = $c->status;
	echo "P";
	switch ($c->poll()) {
		case pq\Connection::POLLING_READING:
			echo "R";
			$w = $e = null;
			$r = array($c->socket);
			stream_select($r, $w, $e, NULL);
			break;
		case pq\Connection::POLLING_WRITING:
			echo "W";
			$w = array($c->socket);
			$r = $e = null;
			stream_select($r, $w, $e, null);
			break;
		case pq\Connection::POLLING_FAILED:
			echo "F";
			break 2;
		case pq\Connection::POLLING_OK:
			echo "S";
			break 2;
	}
}
printf("\n%s\n", implode(",", $s));
?>
DONE
--EXPECTREGEX--
Test
(WP(RP)*)+S
(2,)*3(,\d)*,4
DONE
