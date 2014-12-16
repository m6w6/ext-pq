#!/bin/sh -x

set -e

git clone --depth 1 https://github.com/php/pecl-php-raphf $HOME$BUILD_SRC_DIR/raphf
cd $HOME$BUILD_SRC_DIR/raphf

$HOME$BUILD_INSTALL_DIR/bin/phpize
./configure --with-php-config=$HOME$BUILD_INSTALL_DIR/bin/php-config --enable-raphf

make -j2 --quiet install

echo 'extension=raphf.so' > $HOME$BUILD_INSTALL_DIR/conf.d/10-raphf.ini
