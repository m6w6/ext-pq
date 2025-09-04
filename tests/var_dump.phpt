--TEST--
var_dump -- debug_info
--SKIPIF--
<?php
include "_skipif.inc";
?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
var_dump($c);

?>
DONE
--EXPECTF--
Test
object(pq\Connection)#%d (%d) {
  ["status"]=>
  int(0)
  ["transactionStatus"]=>
  int(0)
  ["socket"]=>
  resource(%d) of type (stream)
  ["errorMessage"]=>
  string(0) ""
  ["busy"]=>
  bool(false)
  ["encoding"]=>
  string(%d) "%S"
  ["unbuffered"]=>
  bool(false)
  ["nonblocking"]=>
  bool(false)
  ["db"]=>
  string(%d) "%S"
  ["user"]=>
  string(%d) "%S"
  ["pass"]=>
  string(%d) "%S"
  ["host"]=>
  string(%d) "%S"
  ["port"]=>
  string(%d) "%S"
  ["params"]=>
  array(%d) {
%a
  }
  ["options"]=>
  string(0) ""
  ["eventHandlers"]=>
  array(0) {
  }
  ["listeners"]=>
  array(0) {
  }
  ["converters"]=>
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
  ["defaultAutoConvert"]=>
  int(65535)
  ["libraryVersion"]=>
  string(%d) "%s"
  ["protocolVersion"]=>
  int(%d)
  ["serverVersion"]=>
  string(%d) "%s"
}
DONE
