--TEST--
async exec params
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$t = new pq\Types($c);
$c->execParamsAsync("SELECT \$1,\$2::int4", array(1,2), array($t["int4"]->oid), function ($res) {
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
  int(1)
  ["numCols"]=>
  int(2)
  ["affectedRows"]=>
  int(%d)
  ["fetchType"]=>
  int(0)
  ["autoConvert"]=>
  int(65535)
}
DONE
