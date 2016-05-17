--TEST--
async unbuffered exec
--SKIPIF--
<?php
include "_skipif.inc";
defined("pq\\Result::SINGLE_TUPLE") or die("skip need pq\\Result::SINGLE_TUPLE");
?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->unbuffered = true;
$c->execAsync("SELECT a FROM generate_series(1,3) a", function ($res) {
	var_dump($res);
});
do {
	while ($c->busy) {
		$r = array($c->socket);
		$w = $e = null;
		if (stream_select($r, $w, $e, null)) {
			$c->poll();
		}
	}
} while ($c->getResult());

?>
DONE
--EXPECTF--
Test
object(pq\Result)#%d (9) {
  ["status"]=>
  int(9)
  ["statusMessage"]=>
  string(12) "SINGLE_TUPLE"
  ["errorMessage"]=>
  string(0) ""
  ["diag"]=>
  array(17) {
    ["severity"]=>
    NULL
    ["sqlstate"]=>
    NULL
    ["message_primary"]=>
    NULL
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
    NULL
    ["source_line"]=>
    NULL
    ["source_function"]=>
    NULL
  }
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(0)
  ["fetchType"]=>
  int(0)
  ["autoConvert"]=>
  int(65535)
}
object(pq\Result)#%d (9) {
  ["status"]=>
  int(9)
  ["statusMessage"]=>
  string(12) "SINGLE_TUPLE"
  ["errorMessage"]=>
  string(0) ""
  ["diag"]=>
  array(17) {
    ["severity"]=>
    NULL
    ["sqlstate"]=>
    NULL
    ["message_primary"]=>
    NULL
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
    NULL
    ["source_line"]=>
    NULL
    ["source_function"]=>
    NULL
  }
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(0)
  ["fetchType"]=>
  int(0)
  ["autoConvert"]=>
  int(65535)
}
object(pq\Result)#%d (9) {
  ["status"]=>
  int(9)
  ["statusMessage"]=>
  string(12) "SINGLE_TUPLE"
  ["errorMessage"]=>
  string(0) ""
  ["diag"]=>
  array(17) {
    ["severity"]=>
    NULL
    ["sqlstate"]=>
    NULL
    ["message_primary"]=>
    NULL
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
    NULL
    ["source_line"]=>
    NULL
    ["source_function"]=>
    NULL
  }
  ["numRows"]=>
  int(1)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(0)
  ["fetchType"]=>
  int(0)
  ["autoConvert"]=>
  int(65535)
}
object(pq\Result)#%d (9) {
  ["status"]=>
  int(2)
  ["statusMessage"]=>
  string(9) "TUPLES_OK"
  ["errorMessage"]=>
  string(0) ""
  ["diag"]=>
  array(17) {
    ["severity"]=>
    NULL
    ["sqlstate"]=>
    NULL
    ["message_primary"]=>
    NULL
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
    NULL
    ["source_line"]=>
    NULL
    ["source_function"]=>
    NULL
  }
  ["numRows"]=>
  int(0)
  ["numCols"]=>
  int(1)
  ["affectedRows"]=>
  int(3)
  ["fetchType"]=>
  int(0)
  ["autoConvert"]=>
  int(65535)
}
DONE
