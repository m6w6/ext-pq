#!/bin/sh -x

set -e
TARGET_PHP_REF="PHP-5.6"

mkdir -p $HOME/php
mkdir -p $HOME/php.d
git clone --depth=1 --branch=$TARGET_PHP_REF https://github.com/php/php-src $HOME/php/src

cd $HOME/php/src
./buildconf --force
./configure --prefix=$HOME --with-config-file-scan-dir=$HOME/php.d --disable-all --enable-maintainer-zts --enable-json --with-mhash

make -j2 --quiet install
