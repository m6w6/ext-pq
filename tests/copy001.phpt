--TEST--
copy
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$c = new pq\Connection(PQ_DSN);
$c->exec("DROP TABLE IF EXISTS copy_test; CREATE TABLE copy_test (id serial, line text);");

$file = file(__FILE__);

$in = new pq\COPY($c, "copy_test (line)", pq\COPY::FROM_STDIN, "DELIMITER '\t'");

var_dump(
	$c === $in->connection,
	"copy_test (line)" === $in->expression,
	pq\COPY::FROM_STDIN === $in->direction,
	"DELIMITER '\t'" === $in->options
);

foreach ($file as $i => $line) {
	$in->put(addcslashes($line, "\\\t"));
}
$in->end();

$out = new pq\COPY($c, "copy_test (line)", pq\COPY::TO_STDOUT, "DELIMITER '\t'");

var_dump(
	$c === $out->connection,
	"copy_test (line)" === $out->expression,
	pq\COPY::TO_STDOUT === $out->direction,
	"DELIMITER '\t'" === $out->options
);

while ($out->get($line)) {
	$lines[] = stripcslashes($line);
}

var_dump($file == $lines);

if ($file != $lines) {
	foreach (array_keys(array_diff($file, $lines)) as $idx) {
		var_dump($idx, $file[$idx], $lines[$idx], "##############");
	}
}

$c->exec("DROP TABLE copy_test");

?>
DONE
--EXPECT--
Test
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
DONE

