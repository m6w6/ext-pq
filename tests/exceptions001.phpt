--TEST--
exceptions
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

try {
	new pq\Connection(1,2,3,4);
	foo();
} catch (pq\Exception $e) {
assert($e->getCode() == pq\Exception::INVALID_ARGUMENT, $e->getCode()."!=".pq\Exception::INVALID_ARGUMENT);
}
try {
	new pq\Connection(1,2,3,4);
	foo();
} catch (pq\Exception\InvalidArgumentException $e) {
	assert($e->getCode() == pq\Exception::INVALID_ARGUMENT, $e->getCode()."!=".pq\Exception::INVALID_ARGUMENT);
}

class c extends pq\Connection {
	function __construct() {
	}
	function open($dsn) {
		parent::__construct($dsn);
	}
}
$c = new c;
try {
	$c->reset();
	foo();
} catch (pq\Exception\BadMethodCallException $e) {
	assert($e->getCode() == pq\Exception::UNINITIALIZED, $e->getCode()."!=".pq\Exception::UNINITIALIZED);
}

$c->open(PQ_DSN);
try {
	$c->open(PQ_DSN);
	foo();
} catch (pq\Exception\BadMethodCallException $e) {
	assert($e->getCode() == pq\Exception::BAD_METHODCALL, $e->getCode()."!=".pq\Exception::BAD_METHODCALL);
}

?>
DONE
--EXPECT--
Test
DONE
