# pecl/pq

[![Build Status](https://travis-ci.org/m6w6/ext-pq.svg?branch=master)](https://travis-ci.org/m6w6/ext-pq)

## About:

This is a modern binding to the mature [libpq](http://www.postgresql.org/docs/current/static/libpq.html), the official PostgreSQL C-client library.

### Highlights:

* Nearly 100% support for [asynchronous usage](http://devel-m6w6.rhcloud.com/mdref/pq/pq/Connection/: Asynchronous Usage).
* Extended [type support by pg_type](http://devel-m6w6.rhcloud.com/mdref/pq/pq/Types/: Overview).
* Fetching simple [multi-dimensional array maps](http://devel-m6w6.rhcloud.com/mdref/pq/pq/Result/map).
* Working [Gateway implementation](https://github.com/m6w6/pq-gateway).

## Installation:

This extension is hosted at [PECL](http://pecl.php.net) and can be installed with [PEAR](http://pear.php.net)'s pecl command:

	# pecl install pq

Also, watch out for self-installing [pharext](https://github.com/m6w6/pharext) packages attached to [releases](https://github.com/m6w6/ext-pq/releases).

## Dependencies:

This extension unconditionally depends on the pre-loaded presence of the following PHP extensions:

- [raphf](http://pecl.php.net/package/raphf)
- [spl](http://php.net/spl)

It optionally depends on the following extensions:

* [json](http://php.net/json)


## Documentation:

Documentation is available at http://devel-m6w6.rhcloud.com/mdref/pq/
