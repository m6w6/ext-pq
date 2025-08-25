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
$cur = "8.4";
$job = $gen->github([
"old-matrix" => [
	"PHP" => ["7.4", "8.0", "8.1", "8.2", "8.3"],
	"enable_debug" => "yes",
	"enable_maintainer_zts" => "yes",
	"enable_json" => "yes",
],
"next" => [
    "PHP" => ["master"],
    "enable_debug" => "yes",
    "enable_zts" => "yes",
],
"cur-dbg-zts" => [
    "PHP" => $cur,
    "enable_debug",
    "enable_zts",
],
"cur-cov" => [
    "CFLAGS" => "-O0 -g --coverage",
    "CXXFLAGS" => "-O0 -g --coverage",
    "PHP" => $cur,
]]);
foreach ($job as $id => $env) {
    printf("  %s:\n", $id);
    printf("    name: \"%s (%s)\"\n", $id, $env["PHP"]);
    if ($env["PHP"] == "master") {
        printf("    continue-on-error: true\n");
    }
    printf("    env:\n");
    foreach ($env as $key => $val) {
        printf("      %s: \"%s\"\n", $key, $val);
    }
?>
      PQ_DSN: "postgres:///runner"
    runs-on: ubuntu-24.04
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
        uses: codecov/codecov-action@v5
        with:
          directory: src
<?php endif; ?>

<?php
}
