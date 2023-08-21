# pecl/pq

[![Build Status](https://github.com/m6w6/ext-pq/workflows/ci/badge.svg?branch=master)](https://github.com/m6w6/ext-pq/actions?query=branch%3Amaster+workflow%3Aci)
[![codecov](https://codecov.io/gh/m6w6/ext-pq/branch/master/graph/badge.svg?token=Nku9tz8EMj)](https://codecov.io/gh/m6w6/ext-pq)

## About:

This is a modern binding to the mature [libpq](http://www.postgresql.org/docs/current/static/libpq.html), the official PostgreSQL C-client library.

### Highlights:

* Nearly 100% support for [asynchronous usage](https://mdref.m6w6.name/pq/Connection/:%20Asynchronous%20Usage).
* Extended [type support by pg_type](https://mdref.m6w6.name/pq/Types/:%20Overview).
* Fetching simple [multi-dimensional array maps](https://mdref.m6w6.name/pq/Result/map).
* Working [Gateway implementation](https://github.com/m6w6/pq-gateway).

## Documentation

See the [online markdown reference](https://mdref.m6w6.name/pq).

Known issues are listed in [BUGS](./BUGS) and future ideas can be found in [TODO](./TODO).

## Install

### PECL

	pecl install pq

### PHARext

Watch out for [PECL replicates](https://replicator.pharext.org?pq)
and pharext packages attached to [releases](https://github.com/m6w6/ext-pq/releases).

### Checkout

	git clone github.com:m6w6/ext-pq
	cd ext-pq
	/path/to/phpize
	./configure --with-php-config=/path/to/php-config
	make
	sudo make install

## Dependencies:

This extension unconditionally depends on the pre-loaded presence of the following PHP extensions:

- [raphf](http://pecl.php.net/package/raphf)
- [spl](http://php.net/spl)

It optionally depends on the following extensions:

* [json](http://php.net/json)

## ChangeLog

A comprehensive list of changes can be obtained from the
[PECL website](https://pecl.php.net/package-changelog.php?package=pq).

## License

ext-pq is licensed under the 2-Clause-BSD license, which can be found in
the accompanying [LICENSE](./LICENSE) file.

## Contributing

All forms of contribution are welcome! Please see the bundled
[CONTRIBUTING](./CONTRIBUTING.md) note for the general principles followed.

The list of past and current contributors is maintained in [THANKS](./THANKS).
