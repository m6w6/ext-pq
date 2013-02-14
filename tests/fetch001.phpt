--TEST--
fetch type
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$r = $c->exec("SELECT a,b, NULL as c from generate_series(1,2) a, generate_series(2,4) b");

$r->fetchType = pq\Result::FETCH_ARRAY;
foreach ($r as $k => $v) {
	printf("%s => %s,%s,%s\n", $k, $v[0], $v[1], $v[2]);
	$r->fetchType = (string) $r->fetchType;
}

$r->fetchType = pq\Result::FETCH_ASSOC;
foreach ($r as $k => $v) {
	printf("%s => %s,%s,%s\n", $k, $v["a"], $v["b"], $v["c"]);
	$r->fetchType = (string) $r->fetchType;
}

$r->fetchType = pq\Result::FETCH_OBJECT;
foreach ($r as $k => $v) {
	printf("%s => %s,%s,%s\n", $k, $v->a, $v->b, $v->c);
	$r->fetchType = (string) $r->fetchType;
}

?>
DONE
--EXPECT--
Test
0 => 1,2,
1 => 1,3,
2 => 1,4,
3 => 2,2,
4 => 2,3,
5 => 2,4,
0 => 1,2,
1 => 1,3,
2 => 1,4,
3 => 2,2,
4 => 2,3,
5 => 2,4,
0 => 1,2,
1 => 1,3,
2 => 1,4,
3 => 2,2,
4 => 2,3,
5 => 2,4,
DONE
