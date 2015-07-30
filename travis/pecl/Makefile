export

PHP = 5.6
JOBS ?= 2
PHP_MIRROR ?= http://us1.php.net/distributions/

prefix ?= $(HOME)
exec_prefix ?= $(prefix)
bindir = $(exec_prefix)/bin
srcdir := $(dir $(word $(words $(MAKEFILE_LIST)),$(MAKEFILE_LIST)))

enable_maintainer_zts ?= no
enable_debug ?= no
enable_all ?= no
with_config_file_scan_dir ?= $(prefix)/etc/php.d

with_php_config ?= $(bindir)/php-config
extdir = $(shell test -x $(with_php_config) && $(with_php_config) --extension-dir)

PECL_MIRROR ?= http://pecl.php.net/get/
PECL_EXTENSION ?= $(shell echo $(PECL) | cut -d: -f1)
PECL_SONAME ?= $(if $(shell echo $(PECL) | cut -d: -f2),$(shell echo $(PECL) | cut -d: -f2),$(PECL_EXTENSION))
PECL_VERSION ?= $(shell echo $(PECL) | cut -d: -f3 -s)
PECL_INI = $(with_config_file_scan_dir)/pecl.ini

PHP_VERSION ?= $(shell test -e $(srcdir)/php-versions.json && cat $(srcdir)/php-versions.json | /usr/bin/php $(srcdir)/php-version.php $(PHP))

.PHONY: all php check clean reconf pecl ext test
.SUFFIXES:

all: php

## -- PHP

clean:
	@if test -d $(srcdir)/php-$(PHP_VERSION); then cd $(srcdir)/php-$(PHP_VERSION); make distclean || true; done

check: $(srcdir)/php-versions.json
	@if test -z "$(PHP)"; then echo "No php version specified, e.g. PHP=5.6"; exit 1; fi

reconf: check $(srcdir)/php-$(PHP_VERSION)/configure
	cd $(srcdir)/php-$(PHP_VERSION) && ./configure -C --prefix=$(prefix)

php: check $(bindir)/php

$(srcdir)/php-versions.json: $(srcdir)/php-version.php
	curl -Sso $@ http://php.net/releases/active.php

$(srcdir)/php-$(PHP_VERSION)/configure: | $(srcdir)/php-versions.json
	curl -Ss $(PHP_MIRROR)/php-$(PHP_VERSION).tar.bz2 | tar xj -C $(srcdir)

$(srcdir)/php-$(PHP_VERSION)/Makefile: $(srcdir)/php-$(PHP_VERSION)/configure | $(srcdir)/php-versions.json
	cd $(srcdir)/php-$(PHP_VERSION) && ./configure -C --prefix=$(prefix)

$(srcdir)/php-$(PHP_VERSION)/sapi/cli/php: $(srcdir)/php-$(PHP_VERSION)/Makefile | $(srcdir)/php-versions.json
	cd $(srcdir)/php-$(PHP_VERSION) && make -s -j $(JOBS) V=0 || make

$(bindir)/php: $(srcdir)/php-$(PHP_VERSION)/sapi/cli/php | $(srcdir)/php-versions.json
	cd $(srcdir)/php-$(PHP_VERSION) && make -s install V=0

$(with_config_file_scan_dir):
	mkdir -p $@

## -- PECL

pecl-check:
	@if test -z "$(PECL)"; then echo "No pecl extension specified, e.g. PECL=pecl_http:http"; exit 1; fi

$(PECL_INI): | $(with_config_file_scan_dir)
	touch $@

$(srcdir)/pecl-$(PECL_EXTENSION):
	test -e $@ || ln -s $(CURDIR) $@

$(srcdir)/pecl-$(PECL_EXTENSION)/config.m4:
	mkdir -p $(srcdir)/pecl-$(PECL_EXTENSION)
	curl -Ss $(PECL_MIRROR)/$(PECL_EXTENSION)$(if $(PECL_VERSION),/$(PECL_VERSION)) | tar xz --strip-components 1 -C $(srcdir)/pecl-$(PECL_EXTENSION)

$(srcdir)/pecl-$(PECL_EXTENSION)/configure: $(srcdir)/pecl-$(PECL_EXTENSION)/config.m4
	cd $(srcdir)/pecl-$(PECL_EXTENSION) && $(bindir)/phpize

$(srcdir)/pecl-$(PECL_EXTENSION)/Makefile: $(srcdir)/pecl-$(PECL_EXTENSION)/configure
	cd $(srcdir)/pecl-$(PECL_EXTENSION) && ./configure -C

$(srcdir)/pecl-$(PECL_EXTENSION)/.libs/$(PECL_SONAME).so: $(srcdir)/pecl-$(PECL_EXTENSION)/Makefile
	cd $(srcdir)/pecl-$(PECL_EXTENSION) && make -s -j $(JOBS) V=0 || make
	
$(extdir)/$(PECL_SONAME).so: $(srcdir)/pecl-$(PECL_EXTENSION)/.libs/$(PECL_SONAME).so
	cd $(srcdir)/pecl-$(PECL_EXTENSION) && make -s install V=0
		
pecl: pecl-check php $(extdir)/$(PECL_SONAME).so | $(PECL_INI)
	grep -q extension=$(PECL_SONAME).so $(PECL_INI) || echo extension=$(PECL_SONAME).so >> $(PECL_INI)

ext: pecl-check $(srcdir)/pecl-$(PECL_EXTENSION) pecl

test: php
	REPORT_EXIT_STATUS=1 NO_INTERACTION=1 $(bindir)/php run-tests.php -p $(bindir)/php --show-diff tests
