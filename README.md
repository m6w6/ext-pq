pecl/pq
=======

[![Build Status](https://travis-ci.org/php/pecl-database-pq.svg?branch=master)](https://travis-ci.org/php/pecl-database-pq)

About
-----

This is a modern binding to the mature [libpq](http://www.postgresql.org/docs/current/static/libpq.html), the official PostgreSQL C-client library.

Highlights:

- Nearly 100% support for asynchronous usage.
- Extended type support by pg_type.
- Fetching simple multi-dimensional array maps.
- Working [Gateway implementation](https://github.com/m6w6/pq-gateway).

Installation
------------

This extension is hosted at [PECL](http://pecl.php.net/) and can be installed with [PEAR](http://pear.php.net/)'s `pecl` command:

    # pecl install pq

Dependencies
------------

This extension unconditionally depends on the pre-loaded presence of the following PHP extensions:

- [raphf](http://pecl.php.net/package/raphf)
- [spl](http://php.net/spl)

Documentation
-------------

Documentation is available [here](http://devel-m6w6.rhcloud.com/mdref/pq).
