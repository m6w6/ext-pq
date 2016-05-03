--TEST--
callback sanity
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php 
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->execAsync("select 1; select 2", function($r) use($c) {
	echo "CALLBACK 1\n";
	print_r($r->fetchAll());
	$c->exec("select 'bug'");
	try {
		$c->execAsync("select 3; select 4", function($r) {
			echo "CALLBACK 2\n";
			print_r($r->fetchAll());
		});
	} catch (Exception $e) {
		printf("%s\n", $e->getMessage());
	}
});
$c->exec("select 'end'");
?>
===DONE===
--EXPECT--
Test
CALLBACK 1
Array
(
    [0] => Array
        (
            [0] => 1
        )

)
CALLBACK 1
Array
(
    [0] => Array
        (
            [0] => 2
        )

)
CALLBACK 2
Array
(
    [0] => Array
        (
            [0] => 3
        )

)
CALLBACK 2
Array
(
    [0] => Array
        (
            [0] => 4
        )

)
CALLBACK 2
Array
(
    [0] => Array
        (
            [0] => 3
        )

)
CALLBACK 2
Array
(
    [0] => Array
        (
            [0] => 4
        )

)
===DONE===