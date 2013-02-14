--TEST--
unbuffered result
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
var_dump($c->unbuffered ? true : false);
$c->unbuffered = 1;
var_dump($c->unbuffered);

$c->execAsync("SELECT a from generate_series(1,10) a", function($res) {
	switch ($res->status) {
	case pq\Result::SINGLE_TUPLE:
		printf("%s\n", $res->fetchCol());
		break;
	case pq\Result::TUPLES_OK:
		printf("-> fetching done\n");
		break;
	default:
		printf("!! %s\n", $res->errorMessage);
		break;
	}
});
do {
	while ($c->busy) {
		switch ($c->poll()) {
			case pq\Connection::POLLING_READING:
				$w=$e=array();
				$r=array($c->socket);
				stream:select($r,$w,$e,1);
				break;
			case pq\Connection::POLLING_WRITING:
				$r=$e=array();
				$w=array($c->socket);
				stream_select($r,$w,$e,1);
				break;
		}
	}
} while ($c->getResult());
?>
DONE
--EXPECTF--
Test
bool(false)
bool(true)
1
2
3
4
5
6
7
8
9
10
-> fetching done
DONE
