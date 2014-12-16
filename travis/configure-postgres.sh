#!/bin/sh -x

PQ_CREATE_DB_SQL="CREATE DATABASE $PG_TEST_DB_NAME;"
PQ_DSN="postgres://$PG_TEST_DB_USER@localhost/$PG_TEST_DB_NAME"

psql -c "$PQ_CREATE_DB_SQL" -U $PG_TEST_DB_USER
echo "<?php const PQ_DSN = '$PQ_DSN';" > ./tests/_setup.inc
