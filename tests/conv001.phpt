--TEST--
converter
--SKIPIF--
<?php
include "_skipif.inc";
_ext("json");
?>
--INI--
date.timezone=UTC
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

abstract class Converter implements pq\Converter
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
	
	function convertFromString($string, $type) {
		return eval("return [$string];");
	}
	
	function convertToString($data, $type) {
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
	
	function convertFromString($string, $type) {
		return array_map("intval", explode(" ", $string));
	}
	
	function convertToString($data, $type) {
		return implode(" ", $data);
	}
}

class JSONConverter extends Converter
{
	function convertTypes() {
		return [ $this->types["json"]->oid ];
	}
	
	function convertFromString($string, $type) {
		return json_decode($string);
	}
	
	function convertToString($data, $type) {
		return json_encode($data);
	}
}

class Text {
	private $data;
	function __construct($data) {
		$this->data = $data;
	}
	function __toString() {
		return (string) $this->data;
	}
}

$c = new pq\Connection(PQ_DSN);
$c->exec("CREATE EXTENSION IF NOT EXISTS hstore");
$t = new pq\Types($c);

$c->setConverter(new HStoreConverter($t));
$c->setConverter(new IntVectorConverter($t));
if (!defined("pq\\Types::JSON")) {
	$c->setConverter(new JSONConverter($t));
}
$r = $c->execParams("SELECT \$1 as hs, \$2 as iv, \$3 as oids, \$4 as js, \$5 as ia, \$6 as ta, \$7 as ba, \$8 as da, \$9 as dbl, \$10 as bln, ".
		"\$11 as dt1, \$12 as dt2, \$13 as dt3, \$14 as dt4, \$15 as dt5, \$16 as dt6, \$17 as dt7, \$18 as dt8, \$19 as txta ",
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
		),
		// arrays
		array(array(array(1,2,3))),
		array(array("a\"","b}",null)),
		array(true,false),
		array(1.1,2.2),
		// double
		123.456,
		// bool
		true,
		// datetimes
		new pq\Datetime,
		new pq\Datetime,
		new pq\Datetime,
		new pq\Datetime,
		new pq\Datetime,
		new pq\Datetime,
		new pq\Datetime,
		new pq\Datetime,
		[new Text(0), new Text(" or "), new Text(true)],
	),
	array(
		$t["hstore"]->oid,
		$t["int2vector"]->oid,
		$t["oidvector"]->oid,
		$t["json"]->oid,
		$t["_int4"]->oid,
		$t["_text"]->oid,
		$t["_bool"]->oid,
		$t["_float8"]->oid,
		$t["float4"]->oid,
		$t["bool"]->oid,
		$t["date"]->oid,
		$t["abstime"]->oid,
		$t["timestamp"]->oid,
		$t["timestamptz"]->oid,
		$t["date"]->oid,
		$t["abstime"]->oid,
		$t["timestamp"]->oid,
		$t["timestamptz"]->oid,
		$t["_text"]->oid
	)
);

var_dump($r->fetchAll());

?>
Done
--EXPECTF--
Test
array(1) {
  [0]=>
  array(%d) {
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
    [4]=>
    array(1) {
      [0]=>
      array(1) {
        [0]=>
        array(3) {
          [0]=>
          int(1)
          [1]=>
          int(2)
          [2]=>
          int(3)
        }
      }
    }
    [5]=>
    array(1) {
      [0]=>
      array(3) {
        [0]=>
        string(2) "a""
        [1]=>
        string(2) "b}"
        [2]=>
        NULL
      }
    }
    [6]=>
    array(2) {
      [0]=>
      bool(true)
      [1]=>
      bool(false)
    }
    [7]=>
    array(2) {
      [0]=>
      float(1.1)
      [1]=>
      float(2.2)
    }
    [8]=>
    float(123.456)
    [9]=>
    bool(true)
    [10]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(5) "Y-m-d"
      ["date"]=>
      string(26) "%d-%d-%d 00:00:00.000000"
      ["timezone_type"]=>
      int(3)
      ["timezone"]=>
      string(3) "UTC"
    }
    [11]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(11) "Y-m-d H:i:s"
      ["date"]=>
      string(26) "%d-%d-%d %d:%d:%d.000000"
      ["timezone_type"]=>
      int(1)
      ["timezone"]=>
      string(%d) "%s"
    }
    [12]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(13) "Y-m-d H:i:s.u"
      ["date"]=>
      string(26) "%d-%d-%d %d:%d:%d.000000"
      ["timezone_type"]=>
      int(3)
      ["timezone"]=>
      string(3) "UTC"
    }
    [13]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(14) "Y-m-d H:i:s.uO"
      ["date"]=>
      string(26) "%d-%d-%d %d:%d:%d.000000"
      ["timezone_type"]=>
      int(1)
      ["timezone"]=>
      string(%d) "%s"
    }
    [14]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(5) "Y-m-d"
      ["date"]=>
      string(26) "%d-%d-%d 00:00:00.000000"
      ["timezone_type"]=>
      int(3)
      ["timezone"]=>
      string(3) "UTC"
    }
    [15]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(11) "Y-m-d H:i:s"
      ["date"]=>
      string(26) "%d-%d-%d %d:%d:%d.000000"
      ["timezone_type"]=>
      int(1)
      ["timezone"]=>
      string(%d) "%s"
    }
    [16]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(13) "Y-m-d H:i:s.u"
      ["date"]=>
      string(26) "%d-%d-%d %d:%d:%d.000000"
      ["timezone_type"]=>
      int(3)
      ["timezone"]=>
      string(3) "UTC"
    }
    [17]=>
    object(pq\DateTime)#%d (4) {
      ["format"]=>
      string(14) "Y-m-d H:i:s.uO"
      ["date"]=>
      string(26) "%d-%d-%d %d:%d:%d.000000"
      ["timezone_type"]=>
      int(1)
      ["timezone"]=>
      string(%d) "%s"
    }
    [18]=>
    array(3) {
      [0]=>
      string(1) "0"
      [1]=>
      string(4) " or "
      [2]=>
      string(1) "1"
    }
  }
}
Done
