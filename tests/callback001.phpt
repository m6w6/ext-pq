--TEST--
callback sanity
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php 
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->execAsync("select 1; select 2", function($r) {
	print_r($r->fetchAll());
});
$c->exec("select 3");

?>
===DONE===
--EXPECT--
Test
Array
(
    [0] => Array
        (
            [0] => 1
        )

)
Array
(
    [0] => Array
        (
            [0] => 2
        )

)
===DONE===
