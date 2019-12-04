#!/usr/bin/env php
# autogenerated file; do not edit
language: c
sudo: false

addons:
 postgresql: 9.4
 apt:
  packages:
   - php-cli
   - php-pear
   - valgrind

cache:
 directories:
  - $HOME/cache

before_cache:
 - find $HOME/cache -name '*.gcda' -o -name '*.gcno' -delete

env:
 global:
  - PQ_DSN="postgres://postgres@localhost/test"
 matrix:
<?php

$gen = include "./travis/pecl/gen-matrix.php";
$cur = "7.4";
$env = $gen([
	"PHP" => ["7.0", "7.1", "7.2", "7.3", "master"],
	"enable_debug" => "yes",
	"enable_maintainer_zts" => "yes",
	"enable_json" => "yes",
], [
	"PHP" => $cur,
	"enable_json" => "no",
], [
	"PHP" => $cur,
	"enable_json" => "yes",
	"enable_debug",
	"enable_maintainer_zts"
], [
	"PHP" => $cur,
	"enable_json" => "yes",
	"CFLAGS" => "'-O0 -g --coverage'",
	"CXXFLAGS" => "'-O0 -g --coverage'",
]);

foreach ($env as $g) {
	foreach ($g as $e) {
		printf("  - %s\n", $e);
	}
}
?>

matrix:
 fast_finish: true
 allow_failures:
<?php
$allow_failures = array_merge( ... array_map(function($a) {
	return preg_grep('/^PHP=(master) /', $a);
}, $env));
foreach ($allow_failures as $e) {
	printf("  - env: %s\n", $e);
}
?>

install:
 - |
   if test "$PHP" = master; then \
     make -f travis/pecl/Makefile reconf; \
     make -f travis/pecl/Makefile pecl-rm pecl-clean PECL=raphf:raphf:2.0.0; \
   fi
 - make -f travis/pecl/Makefile php || make -f travis/pecl/Makefile clean php
 - make -f travis/pecl/Makefile pecl PECL=raphf:raphf:2.0.0

before_script:
 - psql -U postgres -c "CREATE DATABASE test"

script:
 - make -f travis/pecl/Makefile ext PECL=pq
 - make -f travis/pecl/Makefile test

after_success:
 - test -n "$CFLAGS" && cd src/.libs && bash <(curl -s https://codecov.io/bash) -X xcode -X coveragepy
