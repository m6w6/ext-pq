#!/bin/sh
CWD=$(dirname $0)
awk -f $CWD/php_pq_type.awk >$CWD//php_pq_type.h \
	</usr/include/postgresql/server/catalog/pg_type.h
