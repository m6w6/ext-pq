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
try {
	$c->execAsync("select 3; select 4", function($r) {
		
	});
} catch (Exception $e) {
	printf("%s\n", $e->getMessage());
}
$c->exec("");
?>
===DONE===
--EXPECT--
Test
Failed to execute query (another command is already in progress)
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
