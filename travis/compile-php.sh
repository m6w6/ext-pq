#!/bin/sh -x

set -e

mkdir -p $HOME$BUILD_SRC_DIR
mkdir -p $HOME$BUILD_INSTALL_DIR/conf.d

git clone --depth=1 --branch=$PHP_TARGET_REF https://github.com/php/php-src $HOME$BUILD_SRC_DIR/php-src

cd $HOME$BUILD_SRC_DIR/php-src
./buildconf --force
./configure --quiet \
  --prefix=$HOME$BUILD_INSTALL_DIR \
  --with-config-file-scan-dir=$HOME$BUILD_INSTALL_DIR/conf.d \
  --disable-all \
  $PHP_CONFIGURE_OPTS \
  $PHP_EXTENSIONS

make -j2 --quiet install
