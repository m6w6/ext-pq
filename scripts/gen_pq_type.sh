#!/bin/sh
CWD=$(dirname $0)
$CWD/php_pq_type-pg11.php $1 >$CWD/../php_pq_type.h
