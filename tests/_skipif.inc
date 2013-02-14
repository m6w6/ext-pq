<?php
function _ext($ext) {
	extension_loaded($ext) or die("skip $ext not loaded");
}
_ext("pq");
include "_setup.inc";
defined("PQ_DSN") or die("skip PQ_DSN undefined");
try {
	new pq\Connection(PQ_DSN);
} catch (pq\Exception $e) {
	die("skip could not connect to PQ_DSN ".$e->getMessage());
}
