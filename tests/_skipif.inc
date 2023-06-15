<?php
function _ext($ext) {
	extension_loaded($ext) or die("skip $ext not loaded");
}
_ext("pq");
include "_setup.inc";
defined("PQ_DSN") or die("skip PQ_DSN undefined");
try {
	$c = new pq\Connection(PQ_DSN);
	if (defined("SERVER_MIN") && version_compare(SERVER_MIN, $c->serverVersion) > 0) {
		die("skip server {$c->serverVersion} is too old, needed " . SERVER_MIN);
	}
} catch (pq\Exception $e) {
	die("skip could not connect to PQ_DSN ".$e->getMessage());
}
