--TEST--
converter
--SKIPIF--
<?php
include "_skipif.inc";
?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

abstract class Converter implements pq\ConverterInterface
{
	protected $types;
	
	function __construct(\pq\Types $types) {
		$this->types = $types;
	}
}

class HStoreConverter extends Converter
{
	function convertTypes() {
		return [ $this->types["hstore"]->oid ];
	}
	
	function convertFromString($string) {
		return eval("return [$string];");
	}
	
	function convertToString($data) {
		$string = "";
		foreach ($data as $k => $v) {
			if (isset($v)) {
				$string .= sprintf("\"%s\"=>\"%s\",", addslashes($k), addslashes($v));
			} else {
				$string .= sprintf("\"%s\"=>NULL,", addslashes($k));
			}
		}
		return $string;
	}
}

class IntVectorConverter extends Converter
{
	function convertTypes() {
		return [ 
			$this->types["int2vector"]->oid, 
			$this->types["oidvector"]->oid
		];
	}
	
	function convertFromString($string) {
		return array_map("intval", explode(" ", $string));
	}
	
	function convertToString($data) {
		return implode(" ", $data);
	}
}

class JSONConverter extends Converter
{
	function convertTypes() {
		return [ $this->types["json"]->oid ];
	}
	
	function convertFromString($string) {
		return json_decode($string);
	}
	
	function convertToString($data) {
		return json_encode($data);
	}
}

$c = new pq\Connection(PQ_DSN);
$c->exec("CREATE EXTENSION IF NOT EXISTS hstore");
$t = new pq\Types($c);

$c->setConverter(new HStoreConverter($t));
$c->setConverter(new IntVectorConverter($t));
$c->setConverter(new JSONConverter($t));

$r = $c->execParams("SELECT \$1 as hs, \$2 as iv, \$3 as oids, \$4 as js",
	array(
		// hstore
		array(
			"k1" => "v1",
			"k2" => "v2",
			"k3" => null
		),
		// vectors
		array(
			1, 3, 5, 7, 9, 11
		),
		array(
			2345124, 1431341, 1343423
		),
		// JSON
		(object) array(
			"int" => 123,
			"obj" => (object) array(
				"a" => 1,
				"b" => 2,
				"c" => 3,
			),
			"str" => "äüö"
		)
	),
	array(
		$t["hstore"]->oid,
		$t["int2vector"]->oid,
		$t["oidvector"]->oid,
		$t["json"]->oid
	)
);

var_dump($r->fetchAll());

?>
Done
--EXPECTF--
Test
array(1) {
  [0]=>
  array(4) {
    [0]=>
    array(3) {
      ["k1"]=>
      string(2) "v1"
      ["k2"]=>
      string(2) "v2"
      ["k3"]=>
      NULL
    }
    [1]=>
    array(6) {
      [0]=>
      int(1)
      [1]=>
      int(3)
      [2]=>
      int(5)
      [3]=>
      int(7)
      [4]=>
      int(9)
      [5]=>
      int(11)
    }
    [2]=>
    array(3) {
      [0]=>
      int(2345124)
      [1]=>
      int(1431341)
      [2]=>
      int(1343423)
    }
    [3]=>
    object(stdClass)#%d (3) {
      ["int"]=>
      int(123)
      ["obj"]=>
      object(stdClass)#%d (3) {
        ["a"]=>
        int(1)
        ["b"]=>
        int(2)
        ["c"]=>
        int(3)
      }
      ["str"]=>
      string(6) "äüö"
    }
  }
}
Done
