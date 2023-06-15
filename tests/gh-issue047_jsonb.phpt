--TEST--
json conv broken since 2.2.1
--SKIPIF--
<?php
define("SERVER_MIN", "9.4");
include "_skipif.inc";
?>
--INI--
date.timezone=UTC
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->defaultFetchType = \pq\Result::FETCH_ASSOC;

$q = <<<EOF
    SELECT '0'::jsonb UNION SELECT '"text"'::jsonb;
EOF;
$r = $c->exec($q);

var_dump($r->fetchAll());
?>
===DONE===
--EXPECT--
Test
array(2) {
  [0]=>
  array(1) {
    ["jsonb"]=>
    string(4) "text"
  }
  [1]=>
  array(1) {
    ["jsonb"]=>
    int(0)
  }
}
===DONE===
