#!/bin/sh -x

psql -c 'create database pq_test;' -U postgres
echo '<?php const PQ_DSN = "postgres://postgres@localhost/pq_test";' > ./tests/_setup.inc
