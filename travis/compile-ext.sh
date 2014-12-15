#!/bin/sh -x

set -e

$HOME/bin/phpize
./configure --with-php-config=$HOME/bin/php-config --with-pq

make -j2 --quiet install

echo 'extension=pq.so' > $HOME/php.d/20-pq.ini
