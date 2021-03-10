--TEST--
empty result
--SKIPIF--
<?php
include "_skipif.inc";
?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

set_error_handler(function($code, $error, $file, $line) {
	printf("\nWarning: %s in %s on line %d\n", $error, $file, $line);
	return true;
}, E_RECOVERABLE_ERROR);

class r extends pq\Result {
	public $dummy = 2;
}

var_dump(new pq\Result);

echo "Test\n";
$v = get_object_vars(new r);
ksort($v);
var_dump($v);

?>
Done
--EXPECTF--
Test

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d
object(pq\Result)#%d (9) {
  ["status"]=>
  NULL
  ["statusMessage"]=>
  NULL
  ["errorMessage"]=>
  NULL
  ["diag"]=>
  NULL
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
Test

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d

Warning: pq\Result not initialized in %s on line %d
array(10) {
  ["affectedRows"]=>
  int(0)
  ["autoConvert"]=>
  int(65535)
  ["diag"]=>
  NULL
  ["dummy"]=>
  int(2)
  ["errorMessage"]=>
  NULL
  ["fetchType"]=>
  int(0)
  ["numCols"]=>
  int(0)
  ["numRows"]=>
  int(0)
  ["status"]=>
  NULL
  ["statusMessage"]=>
  NULL
}
Done
