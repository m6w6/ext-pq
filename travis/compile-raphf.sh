#!/bin/sh -x

set -e

git clone --depth 1 https://github.com/php/pecl-php-raphf $HOME/raphf
cd $HOME/raphf

$HOME/bin/phpize
./configure --with-php-config=$HOME/bin/php-config --with-pq

make -j2 --quiet install

echo 'extension=raphf.so' > $HOME/php.d/10-raphf.ini
