--TEST--
txn properties
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$t = new pq\Transaction(new pq\Connection(PQ_DSN));
var_dump(
	$t->connection,
	$t->isolation,
	$t->readonly,
	$t->deferrable
);

$t->isolation = pq\Transaction::SERIALIZABLE;
$t->readonly = true;
$t->deferrable = true;
var_dump(
	$t->connection,
	$t->isolation,
	$t->readonly,
	$t->deferrable
);
?>
DONE
--EXPECTF--
Test
object(pq\Connection)#%d (18) {
  ["status"]=>
  int(0)
  ["transactionStatus"]=>
  int(2)
  ["socket"]=>
  resource(%d) of type (stream)
  ["errorMessage"]=>
  string(0) ""
  ["busy"]=>
  bool(false)
  ["encoding"]=>
  string(4) "%s"
  ["unbuffered"]=>
  bool(false)
  ["db"]=>
  string(4) "%S"
  ["user"]=>
  string(4) "%S"
  ["pass"]=>
  string(0) "%S"
  ["host"]=>
  string(0) "%S"
  ["port"]=>
  string(4) "%S"
  ["options"]=>
  string(0) "%S"
  ["eventHandlers"]=>
  array(0) {
  }
  ["defaultFetchType"]=>
  int(0)
  ["defaultTransactionIsolation"]=>
  int(0)
  ["defaultTransactionReadonly"]=>
  bool(false)
  ["defaultTransactionDeferrable"]=>
  bool(false)
}
int(0)
bool(false)
bool(false)
object(pq\Connection)#%d (18) {
  ["status"]=>
  int(0)
  ["transactionStatus"]=>
  int(2)
  ["socket"]=>
  resource(%d) of type (stream)
  ["errorMessage"]=>
  string(0) ""
  ["busy"]=>
  bool(false)
  ["encoding"]=>
  string(4) "%s"
  ["unbuffered"]=>
  bool(false)
  ["db"]=>
  string(4) "%S"
  ["user"]=>
  string(4) "%S"
  ["pass"]=>
  string(0) "%S"
  ["host"]=>
  string(0) "%S"
  ["port"]=>
  string(4) "%S"
  ["options"]=>
  string(0) "%S"
  ["eventHandlers"]=>
  array(0) {
  }
  ["defaultFetchType"]=>
  int(0)
  ["defaultTransactionIsolation"]=>
  int(0)
  ["defaultTransactionReadonly"]=>
  bool(false)
  ["defaultTransactionDeferrable"]=>
  bool(false)
}
int(2)
bool(true)
bool(true)
DONE
