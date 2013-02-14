--TEST--
connection info
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";
$c = new pq\Connection(PQ_DSN);
printf("%s%s%s%s%s%s\n", 
	$c->db,
	$c->user,
	$c->pass,
	$c->host,
	$c->port,
	$c->options
);
?>
DONE
--EXPECTF--
Test
%s
DONE
