<?php
function _ext($ext) {
	extension_loaded($ext) or die("skip $ext not loaded");
}
_ext("pq");
