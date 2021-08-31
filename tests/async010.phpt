--TEST--
asnyc query not cleaned before sync exec
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";
include "_setup.inc";

$c = new pq\Connection(PQ_DSN);

var_dump([
	"async" => $c->execAsync("select clock_timestamp(), pg_sleep(0.1), clock_timestamp()", function($r) {
		var_dump([
			"cb" => $r->fetchRow()
		]);
	})
]);

var_dump([
	"execParams" => $c->execParams("select \$1::int4", [123])->fetchRow()
]);
?>
DONE
--EXPECTF--
Test
array(1) {
  ["async"]=>
  NULL
}
array(1) {
  ["cb"]=>
  array(3) {
    [0]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(14) "Y-m-d H:i:s.uO"
      ["date"]=>
      string(26) "%s"
      ["timezone_type"]=>
      int(1)
      ["timezone"]=>
      string(6) "%s"
    }
    [1]=>
    string(0) ""
    [2]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(14) "Y-m-d H:i:s.uO"
      ["date"]=>
      string(26) "%s"
      ["timezone_type"]=>
      int(1)
      ["timezone"]=>
      string(6) "%s"
    }
  }
}
array(1) {
  ["execParams"]=>
  array(1) {
    [0]=>
    int(123)
  }
}
DONE
