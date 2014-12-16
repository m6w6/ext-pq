#!/bin/sh -x

set -e

$HOME$BUILD_INSTALL_DIR/bin/phpize
./configure --with-php-config=$HOME$BUILD_INSTALL_DIR/bin/php-config --with-pq

make -j2 --quiet install

echo 'extension=pq.so' > $HOME$BUILD_INSTALL_DIR/conf.d/20-pq.ini
