--TEST--
cancel
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);

$x = new pq\Cancel($c);

$c->execAsync("SELECT pg_sleep(2)");

$x->cancel();

var_dump($c === $x->connection);
var_dump($c->getResult());
printf("%s\n", $c->errorMessage);
?>
DONE
--EXPECTF--
Test
bool(true)
object(pq\Result)#%d (9) {
  ["status"]=>
  int(7)
  ["statusMessage"]=>
  string(11) "FATAL_ERROR"
  ["errorMessage"]=>
  string(47) "ERROR:  canceling statement due to user request"
  ["diag"]=>
  array(17) {
    ["severity"]=>
    string(5) "ERROR"
    ["sqlstate"]=>
    string(5) "57014"
    ["message_primary"]=>
    string(39) "canceling statement due to user request"
    ["message_detail"]=>
    NULL
    ["message_hint"]=>
    NULL
    ["statement_position"]=>
    NULL
    ["internal_position"]=>
    NULL
    ["internal_query"]=>
    NULL
    ["context"]=>
    NULL
    ["schema_name"]=>
    NULL
    ["table_name"]=>
    NULL
    ["column_name"]=>
    NULL
    ["datatype_name"]=>
    NULL
    ["constraint_name"]=>
    NULL
    ["source_file"]=>
    string(10) "postgres.c"
    ["source_line"]=>
    string(4) "%d"
    ["source_function"]=>
    string(17) "ProcessInterrupts"
  }
  ["numRows"]=>
  int(0)
  ["numCols"]=>
  int(0)
  ["affectedRows"]=>
  int(0)
  ["fetchType"]=>
  int(0)
  ["autoConvert"]=>
  int(65535)
}
ERROR:  canceling statement due to user request
DONE
