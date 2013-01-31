--TEST--
map result
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$r = $c->exec("select (ARRAY['one','two','three','four','five','six','seven','eight','nine','ten'])[a] num, ".
	"round(log(a)::numeric,3) log, round(exp(a)::numeric,3) exp from generate_series(1,10) a");
$r->fetchType = pq\Result::FETCH_OBJECT;

var_dump($r->map());
var_dump($r->map() == $r->map(0));
var_dump($r->map() == $r->map(0, array(0,1,2)));

$r = $c->exec("select * from generate_series(0,1) a, generate_series(0,1) b, generate_series(0,1) c, generate_series(0,1) d ".
	"order by a,b,c,d");
$r->fetchType = pq\Result::FETCH_ARRAY;
var_dump($r->map(array(0,"b",2), "d"));

?>
DONE
--EXPECTF--
Test
object(stdClass)#%d (10) {
  ["one"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(3) "one"
    ["log"]=>
    string(5) "0.000"
    ["exp"]=>
    string(5) "2.718"
  }
  ["two"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(3) "two"
    ["log"]=>
    string(5) "0.301"
    ["exp"]=>
    string(5) "7.389"
  }
  ["three"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(5) "three"
    ["log"]=>
    string(5) "0.477"
    ["exp"]=>
    string(6) "20.086"
  }
  ["four"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(4) "four"
    ["log"]=>
    string(5) "0.602"
    ["exp"]=>
    string(6) "54.598"
  }
  ["five"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(4) "five"
    ["log"]=>
    string(5) "0.699"
    ["exp"]=>
    string(7) "148.413"
  }
  ["six"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(3) "six"
    ["log"]=>
    string(5) "0.778"
    ["exp"]=>
    string(7) "403.429"
  }
  ["seven"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(5) "seven"
    ["log"]=>
    string(5) "0.845"
    ["exp"]=>
    string(8) "1096.633"
  }
  ["eight"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(5) "eight"
    ["log"]=>
    string(5) "0.903"
    ["exp"]=>
    string(8) "2980.958"
  }
  ["nine"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(4) "nine"
    ["log"]=>
    string(5) "0.954"
    ["exp"]=>
    string(8) "8103.084"
  }
  ["ten"]=>
  object(stdClass)#%d (3) {
    ["num"]=>
    string(3) "ten"
    ["log"]=>
    string(5) "1.000"
    ["exp"]=>
    string(9) "22026.466"
  }
}
bool(true)
bool(true)
array(2) {
  [0]=>
  array(2) {
    [0]=>
    array(2) {
      [0]=>
      array(1) {
        [3]=>
        string(1) "1"
      }
      [1]=>
      array(1) {
        [3]=>
        string(1) "1"
      }
    }
    [1]=>
    array(2) {
      [0]=>
      array(1) {
        [3]=>
        string(1) "1"
      }
      [1]=>
      array(1) {
        [3]=>
        string(1) "1"
      }
    }
  }
  [1]=>
  array(2) {
    [0]=>
    array(2) {
      [0]=>
      array(1) {
        [3]=>
        string(1) "1"
      }
      [1]=>
      array(1) {
        [3]=>
        string(1) "1"
      }
    }
    [1]=>
    array(2) {
      [0]=>
      array(1) {
        [3]=>
        string(1) "1"
      }
      [1]=>
      array(1) {
        [3]=>
        string(1) "1"
      }
    }
  }
}
DONE

