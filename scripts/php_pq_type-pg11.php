#!/usr/bin/env php
<?php

/*
 * Generate php_pq_type.h from
 * postgresql.git/src/include/catalog/pg_type.dat
 *
 * Since PgSQL-11
 */

$dir = $argv[1] ?? __DIR__."/../../postgresql.git";
$dat = file_get_contents($dir . "/src/include/catalog/pg_type.dat");
$typ = [];
$arr = [];

if (!$dat) {
	exit(1);
}
if (!preg_match_all('/{(.*?)}/s', $dat, $matches)) {
	fprintf(STDERR, "Failed to find entries in pg_type.dat\n");
	exit(1);
}

foreach ($matches[1] as $set_data) {
	if (!preg_match_all('/(?P<key>\w+) => \'(?P<val>(?:[^\']|(?<=\\\\)\')+)\'/', $set_data, $set_matches)) {
		fprintf(STDERR, "Failed matching key value pairs in set: '%s'\n", $set_data);
		continue;
	}
	$set = array_combine($set_matches["key"], $set_matches["val"]);
	$ucn = strtoupper($set["typname"]);
	$typ[$set["oid"]] = $ucn;

	if (isset($set["array_type_oid"])) {
		$arr[$set["array_type_oid"]] = $set["oid"];
		$typ[$set["array_type_oid"]] = $ucn . "ARRAY";
	}
	if (isset($set["typdelim"])) {
		$delims[$set["oid"]] = $delims[$set["array_type_oid"]] = $set["typdelim"];
	}
}

ksort($typ, SORT_NUMERIC);
ksort($arr, SORT_NUMERIC);
?>

/* Generated file. See scripts/gen_pq_type-pq11.php */

#ifndef PHP_PQ_TYPE
# define PHP_PQ_TYPE(t,o)
#endif

<?php foreach ($typ as $oid => $ucn) : ?>
#ifndef PHP_PQ_OID_<?=$ucn?>

# define PHP_PQ_OID_<?=$ucn?> <?=$oid?>

#endif
PHP_PQ_TYPE("<?=$ucn?>", <?=$oid?>)
<?php endforeach; ?>

#ifndef PHP_PQ_TYPE_IS_ARRAY
# define PHP_PQ_TYPE_IS_ARRAY(oid) ( \
	0 \
<?php foreach ($arr as $oid => $type) : ?>
	||	((oid) == <?=$oid?>) \
<?php endforeach; ?>
)
#endif

#ifndef PHP_PQ_TYPE_OF_ARRAY
# define PHP_PQ_TYPE_OF_ARRAY(oid) ( \
<?php foreach ($arr as $oid => $type) : ?>
	(oid) == <?=$oid?> ? <?=$type?> : \
<?php endforeach; ?>
	0 \
)
#endif

#ifndef PHP_PQ_DELIM_OF_ARRAY
# define PHP_PQ_DELIM_OF_ARRAY(oid) ((char) ( \
<?php foreach ($delims as $oid => $delim) : ?>
	(oid) == <?=$oid?> ? '<?=$delim?>' : \
<?php endforeach; ?>
	',' \
))
#endif
