<?php
function _ext($ext) {
	extension_loaded($ext) or die("skip $ext not loaded");
}
_ext("pq");
include "_setup.inc";
defined("PQ_DSN") or die("skip PG_DSN undefined");
