<?php 

$version = $argv[1];
$versions = json_decode(stream_get_contents(STDIN), 1);

list($major) = explode(".", $version, 2);

if (isset($versions[$major][$version])) {
	printf("%s\n", $versions[$major][$version]["version"]);
}
