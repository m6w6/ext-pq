#!/bin/sh
CWD=$(dirname $0)
$CWD/php_pq_type-pg11.php >$CWD/../php_pq_type.h
