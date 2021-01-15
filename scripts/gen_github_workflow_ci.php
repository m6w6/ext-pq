#!/usr/bin/env php
<?php echo "# generated file; do not edit!\n"; ?>

name: ci
on:
  workflow_dispatch:
  push:
  pull_request:

jobs:
<?php

$gen = include __DIR__ . "/ci/gen-matrix.php";
$cur = "8.0";
$job = $gen->github([
"old-matrix" => [
	"PHP" => ["7.0", "7.1", "7.2", "7.3", "7.4"],
	"enable_debug" => "yes",
	"enable_maintainer_zts" => "yes",
	"enable_json" => "yes",
	"enable_spl" => "yes",
], 
"master" => [
    "PHP" => ["master"],
    "enable_debug" => "yes",
    "enable_zts" => "yes",
    "enable_spl" => "yes",
], 
"cur-none" => [
    "PHP" => $cur,
], 
"cur-dbg-zts" => [
    "PHP" => $cur,
    "enable_debug",
    "enable_zts",
    "enable_spl" => "yes",
], 
"cur-cov" => [
    "CFLAGS" => "-O0 -g --coverage",
    "CXXFLAGS" => "-O0 -g --coverage",
    "PHP" => $cur,
    "enable_spl" => "yes",
]]);
foreach ($job as $id => $env) {
    printf("  %s:\n", $id);
    printf("    name: %s\n", $id);
    if ($env["PHP"] == "master") {
        printf("    continue-on-error: true\n");
    }
    printf("    env:\n");
    foreach ($env as $key => $val) {
        printf("      %s: \"%s\"\n", $key, $val);
    }
?>
    runs-on: ubuntu-20.04
    steps:
      - uses: actions/checkout@v2
        with:
          submodules: true
      - name: Install
        run: |
          sudo apt-get install -y \
            php-cli \
            php-pear \
            libpq-dev \
            re2c
      - name: Prepare
        run: |
          make -f scripts/ci/Makefile php || make -f scripts/ci/Makefile clean php
          make -f scripts/ci/Makefile pecl PECL=m6w6/ext-raphf.git:raphf:master
      - name: Build
        run: |
          make -f scripts/ci/Makefile ext PECL=pq
      - name: Prepare Test
        run: |
          sudo systemctl start postgresql
          sudo -u postgres createuser --login runner
          sudo -u postgres createdb -O runner runner
      - name: Test
        run: |
          make -f scripts/ci/Makefile test
<?php if (isset($env["CFLAGS"]) && strpos($env["CFLAGS"], "--coverage") != false) : ?>
      - name: Coverage
        if: success()
        run: |
          cd src/.libs
          bash <(curl -s https://codecov.io/bash) -X xcode -X coveragepy
<?php endif; ?>

<?php
}
